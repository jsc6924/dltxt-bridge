#pragma once

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

#if __has_include(<uchardet/uchardet.h>)
#include <uchardet/uchardet.h>
#elif __has_include(<uchardet.h>)
#include <uchardet.h>
#else
#error "uchardet header not found"
#endif

namespace bridge_documents {

struct Position {
    std::size_t line = 0;
    std::size_t character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct TextChange {
    std::optional<Range> range;
    std::u16string text;
};

inline std::string percent_decode(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());

    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const int hi = hex_value(text[index + 1]);
            const int lo = hex_value(text[index + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                index += 2;
                continue;
            }
        }

        decoded.push_back(text[index]);
    }

    return decoded;
}

inline std::filesystem::path file_path_from_uri(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (uri.rfind(prefix, 0) != 0) {
        return std::filesystem::path(percent_decode(uri));
    }

    std::string decoded = percent_decode(uri.substr(prefix.size()));
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }

    return std::filesystem::path(decoded);
}

inline std::string file_uri_from_path(const std::filesystem::path& path) {
    std::string generic = path.lexically_normal().generic_string();
#ifdef _WIN32
    if (generic.size() >= 2 && generic[1] == ':') {
        generic.insert(generic.begin(), '/');
    }
#endif
    return "file://" + generic;
}

inline std::u16string normalize_newlines(std::u16string_view text) {
    std::u16string normalized;
    normalized.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char16_t ch = text[index];
        if (ch == u'\r') {
            if (index + 1 < text.size() && text[index + 1] == u'\n') {
                ++index;
            }
            normalized.push_back(u'\n');
            continue;
        }

        normalized.push_back(ch);
    }

    return normalized;
}

inline std::vector<std::u16string> split_lines(std::u16string_view text) {
    std::vector<std::u16string> lines;
    std::size_t start = 0;

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != u'\n' && text[index] != u'\r') {
            continue;
        }

        lines.emplace_back(text.substr(start, index - start));
        if (text[index] == u'\r' && index + 1 < text.size() && text[index + 1] == u'\n') {
            ++index;
        }
        start = index + 1;
    }

    if (start <= text.size()) {
        lines.emplace_back(text.substr(start));
    }

    if (lines.empty()) {
        lines.emplace_back();
    }

    return lines;
}

inline std::string read_file_bytes(const std::filesystem::path& file_path) {
    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open file: " + file_path.string());
    }

    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

inline std::string bom_encoding(std::string_view bytes) {
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return "UTF-8";
    }

    if (bytes.size() >= 4
        && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE
        && static_cast<unsigned char>(bytes[2]) == 0x00
        && static_cast<unsigned char>(bytes[3]) == 0x00) {
        return "UTF-32LE";
    }

    if (bytes.size() >= 4
        && static_cast<unsigned char>(bytes[0]) == 0x00
        && static_cast<unsigned char>(bytes[1]) == 0x00
        && static_cast<unsigned char>(bytes[2]) == 0xFE
        && static_cast<unsigned char>(bytes[3]) == 0xFF) {
        return "UTF-32BE";
    }

    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return "UTF-16LE";
    }

    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFE
        && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        return "UTF-16BE";
    }

    return "";
}

inline std::string normalize_detected_encoding(std::string encoding) {
    if (encoding.empty() || encoding == "unknown") {
        return "UTF-8";
    }

    if (encoding == "ASCII") {
        return "UTF-8";
    }

    return encoding;
}

inline std::string detect_encoding(std::string_view bytes) {
    if (bytes.empty()) {
        return "UTF-8";
    }

    const std::string bom = bom_encoding(bytes);
    if (!bom.empty()) {
        return bom;
    }

    uchardet_t detector = uchardet_new();
    if (detector == nullptr) {
        throw std::runtime_error("Failed to create uchardet detector");
    }

    const int handle_result = uchardet_handle_data(detector, bytes.data(), static_cast<size_t>(bytes.size()));
    if (handle_result != 0) {
        uchardet_delete(detector);
        throw std::runtime_error("uchardet_handle_data failed");
    }

    uchardet_data_end(detector);
    const char* charset = uchardet_get_charset(detector);
    const std::string encoding = charset != nullptr ? charset : "";
    uchardet_delete(detector);
    return normalize_detected_encoding(encoding);
}

inline std::string convert_bytes(std::string_view input, const char* from_encoding, const char* to_encoding) {
#ifdef _WIN32
    (void)input;
    (void)from_encoding;
    (void)to_encoding;
    throw std::runtime_error("convert_bytes is not available on Windows");
#else
    iconv_t converter = iconv_open(to_encoding, from_encoding);
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        throw std::runtime_error("iconv_open failed");
    }

    std::string output((input.size() + 1U) * 4U + 16U, '\0');
    std::size_t input_left = input.size();
    char* input_buffer = const_cast<char*>(input.data());
    char* output_buffer = output.data();
    std::size_t output_left = output.size();

    while (true) {
        const std::size_t result = iconv(converter, &input_buffer, &input_left, &output_buffer, &output_left);
        if (result != static_cast<std::size_t>(-1)) {
            break;
        }

        if (errno != E2BIG) {
            iconv_close(converter);
            throw std::runtime_error("iconv conversion failed");
        }

        const std::size_t used = output.size() - output_left;
        output.resize(output.size() * 2U);
        output_buffer = output.data() + used;
        output_left = output.size() - used;
    }

    iconv_close(converter);
    output.resize(output.size() - output_left);
    return output;
#endif
}

inline std::u16string utf16le_bytes_to_string(std::string_view bytes) {
    if ((bytes.size() % 2U) != 0U) {
        throw std::runtime_error("Invalid UTF-16LE byte count");
    }

    std::u16string text;
    text.reserve(bytes.size() / 2U);
    for (std::size_t index = 0; index < bytes.size(); index += 2U) {
        const char16_t value = static_cast<char16_t>(
            static_cast<unsigned char>(bytes[index])
            | (static_cast<unsigned char>(bytes[index + 1U]) << 8U));
        text.push_back(value);
    }

    if (!text.empty() && text.front() == 0xFEFF) {
        text.erase(text.begin());
    }

    return text;
}

inline std::u16string convert_to_utf16(std::string_view input, const std::string& from_encoding) {
#ifdef _WIN32
    if (input.empty()) {
        return {};
    }

    auto normalized_encoding = from_encoding;
    std::transform(normalized_encoding.begin(), normalized_encoding.end(), normalized_encoding.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    if (normalized_encoding == "UTF-16LE" || normalized_encoding == "UTF-16") {
        return utf16le_bytes_to_string(input);
    }

    if (normalized_encoding == "UTF-16BE") {
        std::string swapped;
        swapped.resize(input.size());
        for (std::size_t index = 0; index + 1 < input.size(); index += 2U) {
            swapped[index] = input[index + 1U];
            swapped[index + 1U] = input[index];
        }
        return utf16le_bytes_to_string(swapped);
    }

    const auto utf32_to_utf16 = [](std::string_view bytes, bool little_endian) {
        if ((bytes.size() % 4U) != 0U) {
            throw std::runtime_error("Invalid UTF-32 byte count");
        }

        std::u16string text;
        for (std::size_t index = 0; index < bytes.size(); index += 4U) {
            std::uint32_t codepoint = 0;
            if (little_endian) {
                codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]))
                    | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1U])) << 8U)
                    | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2U])) << 16U)
                    | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 3U])) << 24U);
            } else {
                codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 3U]))
                    | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2U])) << 8U)
                    | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1U])) << 16U)
                    | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 24U);
            }

            if (codepoint == 0xFEFF) {
                continue;
            }

            if (codepoint <= 0xFFFFU) {
                text.push_back(static_cast<char16_t>(codepoint));
                continue;
            }

            codepoint -= 0x10000U;
            text.push_back(static_cast<char16_t>(0xD800U + ((codepoint >> 10U) & 0x3FFU)));
            text.push_back(static_cast<char16_t>(0xDC00U + (codepoint & 0x3FFU)));
        }

        return text;
    };

    if (normalized_encoding == "UTF-32LE") {
        return utf32_to_utf16(input, true);
    }

    if (normalized_encoding == "UTF-32BE") {
        return utf32_to_utf16(input, false);
    }

    const auto code_page_for_encoding = [](const std::string& encoding) -> UINT {
        if (encoding == "UTF-8") {
            return CP_UTF8;
        }
        if (encoding == "SHIFT_JIS" || encoding == "SHIFT-JIS" || encoding == "CP932" || encoding == "WINDOWS-31J") {
            return 932;
        }
        if (encoding == "GBK" || encoding == "CP936") {
            return 936;
        }
        return CP_UTF8;
    };

    const UINT code_page = code_page_for_encoding(normalized_encoding);
    const int wide_length = MultiByteToWideChar(
        code_page,
        0,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0);
    if (wide_length <= 0) {
        throw std::runtime_error("MultiByteToWideChar size query failed");
    }

    std::wstring wide_text(static_cast<std::size_t>(wide_length), L'\0');
    const int converted_length = MultiByteToWideChar(
        code_page,
        0,
        input.data(),
        static_cast<int>(input.size()),
        wide_text.data(),
        wide_length);
    if (converted_length <= 0) {
        throw std::runtime_error("MultiByteToWideChar conversion failed");
    }

    return std::u16string(wide_text.begin(), wide_text.end());
#else
    return utf16le_bytes_to_string(convert_bytes(input, from_encoding.c_str(), "UTF-16LE"));
#endif
}

inline std::u16string utf8_to_utf16(std::string_view utf8_text) {
    return convert_to_utf16(utf8_text, "UTF-8");
}

inline std::string utf16_to_utf8(std::u16string_view utf16_text) {
#ifdef _WIN32
    if (utf16_text.empty()) {
        return {};
    }

    std::wstring wide_text(utf16_text.begin(), utf16_text.end());
    const int utf8_length = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_text.data(),
        static_cast<int>(wide_text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_length <= 0) {
        throw std::runtime_error("WideCharToMultiByte size query failed");
    }

    std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
    const int converted_length = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_text.data(),
        static_cast<int>(wide_text.size()),
        utf8.data(),
        utf8_length,
        nullptr,
        nullptr);
    if (converted_length <= 0) {
        throw std::runtime_error("WideCharToMultiByte conversion failed");
    }

    return utf8;
#else
    std::string utf16_bytes;
    utf16_bytes.resize(utf16_text.size() * 2U);
    for (std::size_t index = 0; index < utf16_text.size(); ++index) {
        utf16_bytes[index * 2U] = static_cast<char>(utf16_text[index] & 0x00FFU);
        utf16_bytes[index * 2U + 1U] = static_cast<char>((utf16_text[index] >> 8U) & 0x00FFU);
    }

    return convert_bytes(utf16_bytes, "UTF-16LE", "UTF-8");
#endif
}

class TextDocument {
    std::string uri_;
    std::string encoding_;
    std::filesystem::path file_path_;
    std::string file_name_;
    int version_ = 0;
    std::vector<std::u16string> lines_;

public:
    TextDocument() = default;

    TextDocument(
        std::string uri,
        std::string encoding,
        std::filesystem::path file_path,
        int version,
        std::u16string content)
        : uri_(std::move(uri)),
          encoding_(std::move(encoding)),
          file_path_(std::move(file_path)),
          file_name_(file_path_.filename().string()),
          version_(version),
          lines_(split_lines(content)) {}

    const std::string& getUri() const {
        return uri_;
    }

    const std::vector<std::u16string>& getLines() const {
        return lines_;
    }

    const std::u16string& getLine(std::size_t line_number) const {
        if (line_number >= lines_.size()) {
            throw std::out_of_range("line out of range");
        }

        return lines_[line_number];
    }

    std::u16string getContent() const {
        std::u16string content;
        for (std::size_t index = 0; index < lines_.size(); ++index) {
            if (index > 0) {
                content.push_back(u'\n');
            }
            content += lines_[index];
        }

        return content;
    }

    const std::string& getEncoding() const {
        return encoding_;
    }

    const std::filesystem::path& getFilePath() const {
        return file_path_;
    }

    const std::string& getFileName() const {
        return file_name_;
    }

    int getVersion() const {
        return version_;
    }

    void setContent(std::u16string content, int version) {
        version_ = version;
        lines_ = split_lines(content);
    }

    void edit(const TextChange& change, int version) {
        if (!change.range.has_value()) {
            setContent(change.text, version);
            return;
        }

        const auto startLine = change.range->start.line;
        const auto startChar = change.range->start.character;
        const auto endLine = change.range->end.line;
        const auto endChar = change.range->end.character;

        if (startLine > endLine) {
            throw std::out_of_range("edit range start line is after end line");
        }

        if (startLine >= lines_.size() || endLine >= lines_.size()) {
            throw std::out_of_range("edit range line out of bounds");
        }

        if (startChar > lines_[startLine].size() || endChar > lines_[endLine].size()) {
            throw std::out_of_range("edit range character out of bounds");
        }

        if (startLine == endLine && startChar > endChar) {
            throw std::out_of_range("edit range start character is after end character");
        }

        auto patch = split_lines(change.text);
        if (startLine == endLine && patch.size() == 1) {
            auto& line = lines_[startLine];
            line.replace(startChar, endChar - startChar, patch[0]);
            version_ = version;
            return;
        }

        std::u16string newStartLine = lines_[startLine].substr(0, startChar) + patch.front();
        std::u16string newEndLine = patch.back() + lines_[endLine].substr(endChar);

        const auto replacedLineCount = endLine - startLine + 1;
        const auto newLineCount = lines_.size() - replacedLineCount + patch.size();
        auto newLines = std::vector<std::u16string>();
        newLines.reserve(newLineCount);

        for (size_t i = 0; i < startLine; ++i) {
            newLines.push_back(std::move(lines_[i]));
        }

        if (patch.size() == 1) {
            newLines.push_back(std::move(newStartLine) + lines_[endLine].substr(endChar));
        } else {
            newLines.push_back(std::move(newStartLine));
            for (size_t i = 1; i + 1 < patch.size(); ++i) {
                newLines.push_back(std::move(patch[i]));
            }
            newLines.push_back(std::move(newEndLine));
        }

        for (size_t i = endLine + 1; i < lines_.size(); ++i) {
            newLines.push_back(std::move(lines_[i]));
        }

        lines_ = std::move(newLines);
        version_ = version;
    }
};

class TextDocumentLoader {
public:
    static TextDocument load_from_file(const std::filesystem::path& file_path) {
        const std::string bytes = read_file_bytes(file_path);
        const std::string encoding = detect_encoding(bytes);
        return TextDocument(
            file_uri_from_path(file_path),
            encoding,
            file_path,
            0,
            convert_to_utf16(bytes, encoding));
    }

    static TextDocument load_from_lsp(std::string uri, std::string_view utf8_text, int version) {
        const std::filesystem::path file_path = file_path_from_uri(uri);
        return TextDocument(
            std::move(uri),
            "client-decoded",
            file_path,
            version,
            utf8_to_utf16(utf8_text));
    }
};

class DocumentManager {
    std::unordered_map<std::string, TextDocument> open_documents_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_full_sync_at_;
    std::vector<std::string> workspace_folders_;

    bool should_debounce_saved_document_sync(
        const std::string& uri,
        std::chrono::steady_clock::time_point sync_time) const {
        const auto last_sync = last_full_sync_at_.find(uri);
        return last_sync != last_full_sync_at_.end()
            && sync_time - last_sync->second < saved_document_sync_debounce_window;
    }

public:
    static inline constexpr auto saved_document_sync_debounce_window = std::chrono::minutes(1);

    void set_workspace_folders(std::vector<std::string> workspace_folders) {
        workspace_folders_ = std::move(workspace_folders);
    }

    void add_workspace_folder(std::string workspace_folder) {
        if (std::find(workspace_folders_.begin(), workspace_folders_.end(), workspace_folder) == workspace_folders_.end()) {
            workspace_folders_.push_back(std::move(workspace_folder));
        }
    }

    void remove_workspace_folder(const std::string& workspace_folder) {
        workspace_folders_.erase(
            std::remove(workspace_folders_.begin(), workspace_folders_.end(), workspace_folder),
            workspace_folders_.end());
    }

    const std::vector<std::string>& workspace_folders() const {
        return workspace_folders_;
    }

    void upsert_open_document(TextDocument document) {
        last_full_sync_at_.erase(document.getUri());
        open_documents_[document.getUri()] = std::move(document);
    }

    void open_from_lsp(const std::string& uri, std::string_view utf8_text, int version) {
        upsert_open_document(TextDocumentLoader::load_from_lsp(uri, utf8_text, version));
    }

    void apply_lsp_changes(const std::string& uri, int version, const std::vector<TextChange>& changes) {
        auto it = open_documents_.find(uri);
        if (it == open_documents_.end()) {
            fprintf(stderr,
                "DocumentManager::apply_lsp_changes skipped missing uri=%s version=%d change_count=%zu\n",
                uri.c_str(),
                version,
                changes.size());
            return;
        }

        for (const auto& change : changes) {
            try {
                it->second.edit(change, version);
            } catch (const std::exception& e) {
                fprintf(stderr,
                    "DocumentManager::apply_lsp_changes failed uri=%s version=%d: %s\n",
                    uri.c_str(),
                    version,
                    e.what());
                return;
            }
        }
    }

    bool update_saved_document_text(
        const std::string& uri,
        std::string_view utf8_text,
        std::chrono::steady_clock::time_point sync_time = std::chrono::steady_clock::now()) {
        auto it = open_documents_.find(uri);
        if (it == open_documents_.end()) {
            return false;
        }

        if (should_debounce_saved_document_sync(uri, sync_time)) {
            fprintf(stderr,
                "DocumentManager::update_saved_document_text debounced uri=%s within=%lldms\n",
                uri.c_str(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(sync_time - last_full_sync_at_.at(uri)).count()));
            return false;
        }

        it->second.setContent(utf8_to_utf16(utf8_text), it->second.getVersion());
        last_full_sync_at_[uri] = sync_time;
        return true;
    }

    bool update_saved_document_from_disk(
        const std::string& uri,
        std::chrono::steady_clock::time_point sync_time = std::chrono::steady_clock::now()) {
        auto it = open_documents_.find(uri);
        if (it == open_documents_.end()) {
            return false;
        }

        if (should_debounce_saved_document_sync(uri, sync_time)) {
            fprintf(stderr,
                "DocumentManager::update_saved_document_from_disk debounced uri=%s within=%lldms\n",
                uri.c_str(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(sync_time - last_full_sync_at_.at(uri)).count()));
            return false;
        }

        try {
            const TextDocument disk_document = TextDocumentLoader::load_from_file(it->second.getFilePath());
            it->second.setContent(disk_document.getContent(), it->second.getVersion());
            last_full_sync_at_[uri] = sync_time;
            return true;
        } catch (const std::exception& e) {
            fprintf(stderr,
                "DocumentManager::update_saved_document_from_disk failed uri=%s path=%s: %s\n",
                uri.c_str(),
                it->second.getFilePath().string().c_str(),
                e.what());
            return false;
        }
    }

    void close_document(const std::string& uri) {
        open_documents_.erase(uri);
        last_full_sync_at_.erase(uri);
    }

    std::size_t open_document_count() const {
        return open_documents_.size();
    }

    std::vector<std::pair<std::string, std::string>> opened_document_contents_utf8_by_uri() const {
        std::vector<std::pair<std::string, std::string>> documents;
        documents.reserve(open_documents_.size());

        for (const auto& [uri, document] : open_documents_) {
            documents.emplace_back(uri, utf16_to_utf8(document.getContent()));
        }

        std::sort(documents.begin(), documents.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });

        return documents;
    }

    std::optional<std::string> document_content_utf8_by_uri(const std::string& uri) const {
        fprintf(stderr,
            "DocumentManager::document_content_utf8_by_uri lookup uri=%s open_documents=%zu\n",
            uri.c_str(),
            open_documents_.size());
        const auto it = open_documents_.find(uri);
        if (it == open_documents_.end()) {
            fprintf(stderr, "DocumentManager::document_content_utf8_by_uri miss uri=%s\n", uri.c_str());
            for (const auto& [open_uri, document] : open_documents_) {
                (void)document;
                fprintf(stderr, "  open_document uri=%s\n", open_uri.c_str());
            }
            return std::nullopt;
        }
        fprintf(stderr, "DocumentManager::document_content_utf8_by_uri hit uri=%s\n", uri.c_str());
        return utf16_to_utf8(it->second.getContent());
    }

    bool has_document(const std::string& uri) const {
        return open_documents_.find(uri) != open_documents_.end();
    }

    const TextDocument* find_document(const std::string& uri) const {
        const auto it = open_documents_.find(uri);
        return it != open_documents_.end() ? &it->second : nullptr;
    }
};

}  // namespace bridge_documents