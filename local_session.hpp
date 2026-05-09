#pragma once

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace net = boost::asio;
using tcp = net::ip::tcp;

#ifdef _WIN32
using StdioNativeHandle = HANDLE;

inline StdioNativeHandle duplicate_standard_handle(DWORD standard_handle_id) {
    HANDLE source_handle = ::GetStdHandle(standard_handle_id);
    if (source_handle == nullptr || source_handle == INVALID_HANDLE_VALUE) {
        throw boost::system::system_error(
            boost::system::error_code(static_cast<int>(::GetLastError()), boost::system::system_category()));
    }

    HANDLE duplicated_handle = nullptr;
    if (!::DuplicateHandle(
            ::GetCurrentProcess(),
            source_handle,
            ::GetCurrentProcess(),
            &duplicated_handle,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        throw boost::system::system_error(
            boost::system::error_code(static_cast<int>(::GetLastError()), boost::system::system_category()));
    }

    return duplicated_handle;
}
#else
using StdioNativeHandle = int;

inline StdioNativeHandle duplicate_standard_handle(int file_descriptor) {
    const int duplicated_descriptor = ::dup(file_descriptor);
    if (duplicated_descriptor == -1) {
        throw boost::system::system_error(
            boost::system::error_code(errno, boost::system::system_category()));
    }

    return duplicated_descriptor;
}
#endif

class StdioHandle {
    StdioNativeHandle handle_;

public:
    explicit StdioHandle(StdioNativeHandle handle)
        : handle_(handle) {}

    ~StdioHandle() {
        close();
    }

    StdioHandle(const StdioHandle&) = delete;
    StdioHandle& operator=(const StdioHandle&) = delete;

    bool is_open() const {
#ifdef _WIN32
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
        return handle_ >= 0;
#endif
    }

    std::pair<boost::system::error_code, std::size_t> read_some(char* data, std::size_t size) {
#ifdef _WIN32
        DWORD bytes_read = 0;
        const DWORD requested = static_cast<DWORD>((std::min)(size, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!::ReadFile(handle_, data, requested, &bytes_read, nullptr)) {
            return {
                boost::system::error_code(static_cast<int>(::GetLastError()), boost::system::system_category()),
                0
            };
        }

        return {{}, static_cast<std::size_t>(bytes_read)};
#else
        ssize_t bytes_read = -1;
        do {
            bytes_read = ::read(handle_, data, size);
        } while (bytes_read == -1 && errno == EINTR);

        if (bytes_read == -1) {
            return {boost::system::error_code(errno, boost::system::system_category()), 0};
        }

        return {{}, static_cast<std::size_t>(bytes_read)};
#endif
    }

    void write_all(std::string_view data) {
        fprintf(stderr, "StdioHandle::write_all begin: bytes=%zu\n", data.size());
        std::size_t offset = 0;
        while (offset < data.size()) {
#ifdef _WIN32
            DWORD bytes_written = 0;
            const DWORD requested = static_cast<DWORD>((std::min)(data.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            if (!::WriteFile(handle_, data.data() + offset, requested, &bytes_written, nullptr)) {
                throw boost::system::system_error(
                    boost::system::error_code(static_cast<int>(::GetLastError()), boost::system::system_category()));
            }
#else
            ssize_t bytes_written = -1;
            do {
                bytes_written = ::write(handle_, data.data() + offset, data.size() - offset);
            } while (bytes_written == -1 && errno == EINTR);

            if (bytes_written == -1) {
                throw boost::system::system_error(
                    boost::system::error_code(errno, boost::system::system_category()));
            }
#endif

            if (bytes_written == 0) {
                throw boost::system::system_error(net::error::broken_pipe);
            }

            offset += static_cast<std::size_t>(bytes_written);
        }
        fprintf(stderr, "StdioHandle::write_all end: bytes=%zu\n", data.size());
    }

    void close() {
        if (!is_open()) {
            return;
        }

#ifdef _WIN32
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        ::close(handle_);
        handle_ = -1;
#endif
    }
};

inline std::shared_ptr<StdioHandle> open_stdio_input_handle() {
#ifdef _WIN32
    return std::make_shared<StdioHandle>(duplicate_standard_handle(STD_INPUT_HANDLE));
#else
    return std::make_shared<StdioHandle>(duplicate_standard_handle(STDIN_FILENO));
#endif
}

inline std::shared_ptr<StdioHandle> open_stdio_output_handle() {
#ifdef _WIN32
    return std::make_shared<StdioHandle>(duplicate_standard_handle(STD_OUTPUT_HANDLE));
#else
    return std::make_shared<StdioHandle>(duplicate_standard_handle(STDOUT_FILENO));
#endif
}

class LocalSession {
    using WriteTarget = std::variant<std::shared_ptr<tcp::socket>, std::shared_ptr<StdioHandle>>;

    WriteTarget write_target_;
    net::strand<net::any_io_executor> strand_;
    std::deque<std::string> write_queue_;
    bool write_in_progress_ = false;
    std::set<std::string> subscribed_projects_;

public:
    explicit LocalSession(std::shared_ptr<tcp::socket> socket)
        : write_target_(std::move(socket)),
          strand_(net::make_strand(std::get<std::shared_ptr<tcp::socket>>(write_target_)->get_executor())) {}

    LocalSession(net::any_io_executor executor, std::shared_ptr<StdioHandle> output_handle)
        : write_target_(std::move(output_handle)),
          strand_(net::make_strand(executor)) {}

    explicit LocalSession(tcp::socket socket)
        : LocalSession(std::make_shared<tcp::socket>(std::move(socket))) {}

    net::any_io_executor get_executor() const {
        return strand_.get_inner_executor();
    }

    bool is_open() const {
        return std::visit([](const auto& target) {
            return target && target->is_open();
        }, write_target_);
    }

    void close() {
        std::visit([](auto& target) {
            if (!target || !target->is_open()) {
                return;
            }

            boost::system::error_code error;
            using TargetType = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<TargetType, std::shared_ptr<tcp::socket>>) {
                target->shutdown(tcp::socket::shutdown_both, error);
                target->close(error);
            } else {
                target->close();
            }
        }, write_target_);
    }

    net::awaitable<void> write_framed(std::string framed_message) {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (!is_open()) {
            throw boost::system::system_error(net::error::operation_aborted);
        }

        fprintf(stderr,
            "LocalSession::write_framed enqueue: bytes=%zu queue_size_before=%zu write_in_progress=%d\n",
            framed_message.size(),
            write_queue_.size(),
            write_in_progress_ ? 1 : 0);
        write_queue_.push_back(std::move(framed_message));
        if (write_in_progress_) {
            co_return;
        }

        write_in_progress_ = true;

        try {
            while (!write_queue_.empty()) {
                std::string next = std::move(write_queue_.front());
                write_queue_.pop_front();
                fprintf(stderr,
                    "LocalSession::write_framed flush: bytes=%zu remaining_queue=%zu\n",
                    next.size(),
                    write_queue_.size());

                if (auto* socket = std::get_if<std::shared_ptr<tcp::socket>>(&write_target_)) {
                    co_await net::async_write(**socket, net::buffer(next), net::use_awaitable);
                } else {
                    std::get<std::shared_ptr<StdioHandle>>(write_target_)->write_all(next);
                }
            }
        } catch (...) {
            write_in_progress_ = false;
            throw;
        }

        write_in_progress_ = false;
        fprintf(stderr, "LocalSession::write_framed complete\n");
    }

    void register_subscription(const std::string& project_id) {
        subscribed_projects_.insert(project_id);
    }

    bool is_subscribed_to(const std::string& project_id) const {
        return subscribed_projects_.find(project_id) != subscribed_projects_.end();
    }

    void unregister_subscription(const std::string& project_id) {
        subscribed_projects_.erase(project_id);
    }
};
