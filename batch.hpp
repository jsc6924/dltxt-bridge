#pragma once

#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <latch>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "bridge_documents.hpp"

namespace bridge_batch {

struct BatchProcessOptions {
    std::size_t batch_size = 64;
};

class FileBytesView {
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const char* data_ = nullptr;
#endif
    std::string owned_bytes_;

public:
    FileBytesView() = default;

    FileBytesView(const FileBytesView&) = delete;
    FileBytesView& operator=(const FileBytesView&) = delete;

    FileBytesView(FileBytesView&& other) noexcept {
#ifdef _WIN32
        file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        mapping_ = std::exchange(other.mapping_, nullptr);
        data_ = std::exchange(other.data_, nullptr);
#endif
        owned_bytes_ = std::move(other.owned_bytes_);
    }

    FileBytesView& operator=(FileBytesView&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        close();
#ifdef _WIN32
        file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        mapping_ = std::exchange(other.mapping_, nullptr);
        data_ = std::exchange(other.data_, nullptr);
#endif
        owned_bytes_ = std::move(other.owned_bytes_);
        return *this;
    }

    ~FileBytesView() {
        close();
    }

    std::string_view view() const {
#ifdef _WIN32
        if (data_ != nullptr) {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file_, &size)) {
                throw std::runtime_error("GetFileSizeEx failed");
            }
            return std::string_view(data_, static_cast<std::size_t>(size.QuadPart));
        }
#endif
        return owned_bytes_;
    }

    static FileBytesView open(const std::filesystem::path& file_path) {
        FileBytesView result;
#ifdef _WIN32
        result.file_ = ::CreateFileW(
            file_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);

        if (result.file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open file: " + file_path.string());
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(result.file_, &size)) {
            throw std::runtime_error("Failed to get file size: " + file_path.string());
        }

        if (size.QuadPart == 0) {
            return result;
        }

        result.mapping_ = ::CreateFileMappingW(
            result.file_,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr);

        if (result.mapping_ == nullptr) {
            throw std::runtime_error("CreateFileMappingW failed: " + file_path.string());
        }

        void* mapped = ::MapViewOfFile(result.mapping_, FILE_MAP_READ, 0, 0, 0);
        if (mapped == nullptr) {
            throw std::runtime_error("MapViewOfFile failed: " + file_path.string());
        }

        result.data_ = static_cast<const char*>(mapped);
        return result;
#else
        std::ifstream input(file_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open file: " + file_path.string());
        }

        const auto file_size = std::filesystem::file_size(file_path);
        result.owned_bytes_.resize(static_cast<std::size_t>(file_size));
        if (file_size > 0) {
            input.read(result.owned_bytes_.data(), static_cast<std::streamsize>(file_size));
            if (!input) {
                throw std::runtime_error("Failed to read file: " + file_path.string());
            }
        }
        return result;
#endif
    }

private:
    void close() noexcept {
#ifdef _WIN32
        if (data_ != nullptr) {
            ::UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (mapping_ != nullptr) {
            ::CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
#endif
        owned_bytes_.clear();
    }
};

inline bridge_documents::TextDocument load_document_fast(const std::filesystem::path& file_path) {
    FileBytesView bytes = FileBytesView::open(file_path);
    const std::string_view raw = bytes.view();
    const std::string encoding = bridge_documents::detect_encoding(raw);
    return bridge_documents::TextDocument(
        bridge_documents::file_uri_from_path(file_path),
        encoding,
        file_path,
        0,
        bridge_documents::convert_to_utf16(raw, encoding));
}

inline bridge_documents::TextDocument load_document_fast_from_uri(std::string_view uri) {
    return load_document_fast(bridge_documents::file_path_from_uri(std::string(uri)));
}

template <typename Callback>
concept BatchProcessCallback = std::invocable<Callback&, const bridge_documents::TextDocument&, std::size_t>;

template <typename ErrorCallback>
concept BatchProcessErrorCallback =
    std::is_same_v<std::decay_t<ErrorCallback>, std::nullptr_t>
    || std::invocable<ErrorCallback&, const std::string&, std::size_t, std::exception_ptr>;

template <typename Callback, typename ErrorCallback = std::nullptr_t>
void batch_process(
    boost::asio::thread_pool& pool,
    const std::vector<std::string>& uris,
    Callback&& callback,
    BatchProcessOptions options = {},
    ErrorCallback&& on_error = nullptr)
    requires BatchProcessCallback<Callback> && BatchProcessErrorCallback<ErrorCallback> {

    if (uris.empty()) {
        return;
    }

    const std::size_t batch_size = std::max<std::size_t>(1, options.batch_size);

    for (std::size_t base = 0; base < uris.size(); base += batch_size) {
        const std::size_t current_batch_size = std::min(batch_size, uris.size() - base);
        std::latch batch_done(static_cast<std::ptrdiff_t>(current_batch_size));

        for (std::size_t offset = 0; offset < current_batch_size; ++offset) {
            const std::size_t index = base + offset;
            const std::string uri = uris[index];

            boost::asio::post(pool, [&, uri, index]() mutable {
                try {
                    auto document = load_document_fast_from_uri(uri);
                    callback(document, index);
                } catch (...) {
                    if constexpr (!std::is_same_v<std::decay_t<ErrorCallback>, std::nullptr_t>) {
                        on_error(uri, index, std::current_exception());
                    }
                }
                batch_done.count_down();
            });
        }

        batch_done.wait();
    }
}

}  // namespace bridge_batch