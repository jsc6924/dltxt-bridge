#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <deque>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

#include <nlohmann/json.hpp>
#include "local_session.hpp"
#include "bridge_documents.hpp"
#include "bridge_version.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
using json = nlohmann::json;
using RequestId = std::variant<std::monostate, std::int64_t, std::string>;

namespace lsp {
inline constexpr std::size_t max_content_length = 10U * 1024U * 1024U;

inline std::string frame_message(const std::string& payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
}

inline bool is_ignorable_header_block(const std::string& header_block) {
    return !header_block.empty() && std::all_of(header_block.begin(), header_block.end(),
        [](unsigned char ch) {
            return ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t';
        });
}

inline std::string escape_control_chars(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() * 4U);

    constexpr char hex_digits[] = "0123456789ABCDEF";

    for (unsigned char ch : text) {
        switch (ch) {
        case '\r':
            escaped += "\\r";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (std::isprint(ch)) {
                escaped.push_back(static_cast<char>(ch));
            } else {
                escaped += "\\x";
                escaped.push_back(hex_digits[(ch >> 4) & 0x0F]);
                escaped.push_back(hex_digits[ch & 0x0F]);
            }
            break;
        }
    }

    return escaped;
}

template <typename ConstBufferSequence>
inline std::optional<std::size_t> header_block_length(const ConstBufferSequence& buffers) {
    const std::string buffered = beast::buffers_to_string(buffers);
    const std::size_t delimiter_pos = buffered.find("\r\n\r\n");
    if (delimiter_pos == std::string::npos) {
        return std::nullopt;
    }

    return delimiter_pos + 4;
}

template <typename ConstBufferSequence>
inline void log_buffer_snapshot(const char* label, const ConstBufferSequence& buffers) {
    const std::string buffered = beast::buffers_to_string(buffers);
    const std::string escaped = escape_control_chars(buffered);
    fprintf(stderr, "%s (%zu bytes, escaped): [%s]\n", label, buffered.size(), escaped.c_str());
}

inline void append_chunk_to_buffer(beast::flat_buffer& buffer, const char* data, std::size_t size) {
    auto prepared = buffer.prepare(size);
    net::buffer_copy(prepared, net::buffer(data, size));
    buffer.commit(size);
}

inline void log_local_notification(const std::string& method, const json& params = json::object()) {
    fprintf(stderr, "Handled local notification %s: %s\n", method.c_str(), params.dump().c_str());
}

inline void log_local_request(const std::string& method, const json& params = json::object()) {
    fprintf(stderr, "Handled local request %s: %s\n", method.c_str(), params.dump().c_str());
}

template <typename AsyncReadStream>
net::awaitable<std::size_t> read_chunk(
    AsyncReadStream& stream,
    beast::flat_buffer& buffer) {

    std::array<char, 4096> chunk{};
    auto [error, bytes_read] = co_await stream.async_read_some(
        net::buffer(chunk),
        net::as_tuple(net::use_awaitable)
    );

    if (error) {
        fprintf(stderr, "%s\n", error.message().c_str());
        throw boost::system::system_error(error);
    }

    if (bytes_read == 0) {
        fprintf(stderr, "Read returned 0 bytes\n");
        co_return 0;
    }

    append_chunk_to_buffer(buffer, chunk.data(), bytes_read);
    co_return bytes_read;
}

inline std::optional<std::size_t> parse_content_length(const std::string& headers) {
    std::size_t start = 0;
    while (start < headers.size()) {
        std::size_t end = headers.find("\r\n", start);
        if (end == std::string::npos) {
            end = headers.size();
        }

        std::string line = headers.substr(start, end - start);
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        constexpr const char* prefix = "content-length:";
        if (lower.rfind(prefix, 0) == 0) {
            std::string value = line.substr(std::char_traits<char>::length(prefix));
            auto first = value.find_first_not_of(" \t");
            if (first == std::string::npos) {
                return std::nullopt;
            }

            value.erase(0, first);

            try {
                const auto parsed = static_cast<std::size_t>(std::stoull(value));
                if (parsed > max_content_length) {
                    return std::nullopt;
                }

                return parsed;
            } catch (...) {
                return std::nullopt;
            }
        }

        if (end == headers.size()) {
            break;
        }
        start = end + 2;
    }

    return std::nullopt;
}

template <typename AsyncReadStream>
net::awaitable<std::optional<std::string>> read_message(AsyncReadStream& stream, beast::flat_buffer& buffer) {
    for (;;) {
        while (!header_block_length(buffer.data()).has_value()) {
            const auto bytes_read = co_await read_chunk(
                stream,
                buffer
            );

            if (bytes_read == 0) {
                co_return std::nullopt;
            }
        }

        const auto header_bytes = header_block_length(buffer.data());
        if (!header_bytes.has_value()) {
            log_buffer_snapshot("Unable to find header delimiter in buffered bytes", buffer.data());
            co_return std::nullopt;
        }

        std::string header_block = beast::buffers_to_string(beast::buffers_prefix(*header_bytes, buffer.data()));
        buffer.consume(*header_bytes);

        const std::string escaped_header = escape_control_chars(header_block);

        if (is_ignorable_header_block(header_block)) {
            fprintf(stderr, "Ignoring blank header block\n");
            continue;
        }

        auto content_length = parse_content_length(header_block);
        if (!content_length.has_value()) {
            fprintf(stderr, "Invalid header block, unable to parse Content-Length\n");
            co_return std::nullopt;
        }

        fprintf(stderr, "Parsed Content-Length: %zu\n", *content_length);

        while (buffer.size() < *content_length) {
            const auto bytes_read = co_await read_chunk(
                stream,
                buffer
            );

            if (bytes_read == 0) {
                co_return std::nullopt;
            }
        }

        std::string payload = beast::buffers_to_string(beast::buffers_prefix(*content_length, buffer.data()));
        fprintf(stderr, "Received payload (%zu bytes):\n%s\n", payload.size(), payload.c_str());
        buffer.consume(*content_length);
        
        co_return payload;
    }
}

template <typename BlockingReadSomeFn>
std::optional<std::string> read_message_blocking(BlockingReadSomeFn&& read_some, beast::flat_buffer& buffer) {
    for (;;) {
        while (!header_block_length(buffer.data()).has_value()) {
            std::array<char, 4096> chunk{};
            auto [error, bytes_read] = read_some(chunk.data(), chunk.size());

            if (error) {
                fprintf(stderr, "%s\n", error.message().c_str());
                throw boost::system::system_error(error);
            }

            if (bytes_read == 0) {
                fprintf(stderr, "Read returned 0 bytes\n");
                return std::nullopt;
            }

            append_chunk_to_buffer(buffer, chunk.data(), bytes_read);
        }

        const auto header_bytes = header_block_length(buffer.data());
        if (!header_bytes.has_value()) {
            log_buffer_snapshot("Unable to find header delimiter in buffered bytes", buffer.data());
            return std::nullopt;
        }

        std::string header_block = beast::buffers_to_string(beast::buffers_prefix(*header_bytes, buffer.data()));
        buffer.consume(*header_bytes);

        if (is_ignorable_header_block(header_block)) {
            fprintf(stderr, "Ignoring blank header block\n");
            continue;
        }

        auto content_length = parse_content_length(header_block);
        if (!content_length.has_value()) {
            fprintf(stderr, "Invalid header block, unable to parse Content-Length\n");
            return std::nullopt;
        }

        fprintf(stderr, "Parsed Content-Length: %zu\n", *content_length);

        while (buffer.size() < *content_length) {
            std::array<char, 4096> chunk{};
            auto [error, bytes_read] = read_some(chunk.data(), chunk.size());

            if (error) {
                fprintf(stderr, "%s\n", error.message().c_str());
                throw boost::system::system_error(error);
            }

            if (bytes_read == 0) {
                return std::nullopt;
            }

            append_chunk_to_buffer(buffer, chunk.data(), bytes_read);
        }

        std::string payload = beast::buffers_to_string(beast::buffers_prefix(*content_length, buffer.data()));
        fprintf(stderr, "Received payload (%zu bytes):\n%s\n", payload.size(), payload.c_str());
        buffer.consume(*content_length);

        return payload;
    }
}
}

namespace jsonrpc {
inline bool is_trackable_request_id(const RequestId& id) {
    return !std::holds_alternative<std::monostate>(id);
}

inline std::optional<RequestId> request_id_from_json(const json& id) {
    if (id.is_null()) {
        return RequestId{std::monostate{}};
    }

    if (id.is_string()) {
        return RequestId{id.get<std::string>()};
    }

    if (id.is_number_integer()) {
        return RequestId{id.get<std::int64_t>()};
    }

    if (id.is_number_unsigned()) {
        const auto value = id.get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return RequestId{static_cast<std::int64_t>(value)};
        }
    }

    return std::nullopt;
}

inline json request_id_to_json(const RequestId& id) {
    return std::visit(
        [](const auto& value) -> json {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else {
                return value;
            }
        },
        id);
}

inline bool is_valid_jsonrpc_response(const json& message) {
    if (!message.is_object()) {
        return false;
    }

    if (!message.contains("jsonrpc") || message["jsonrpc"] != "2.0") {
        return false;
    }

    if (!message.contains("id")) {
        return false;
    }

    if (message.contains("method")) {
        return false;
    }

    const bool has_result = message.contains("result");
    const bool has_error = message.contains("error");
    return has_result ^ has_error;
}

inline bool is_valid_jsonrpc_notification(const json& message) {
    if (!message.is_object()) {
        return false;
    }

    if (!message.contains("jsonrpc") || message["jsonrpc"] != "2.0") {
        return false;
    }

    if (!message.contains("method")) {
        return false;
    }

    return !message.contains("id");
}

inline json create_response(const json& id, const json& result) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

inline json create_error(const json& id, int code, const std::string& message) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", json{{"code", code}, {"message", message}}}
    };
}

inline json create_request(const json& id, const std::string& method, const json& params = json::object()) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };
}

inline json create_notification(const std::string& method, const json& params = json::object()) {
    return json{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
}
}

namespace bridge_local {
enum class SessionDirective {
    none,
    mark_shutdown_requested,
    close_session,
};

struct RequestContext {
    std::shared_ptr<bridge_documents::DocumentManager> document_manager;
};

struct RequestResult {
    bool forward_to_remote = false;
    std::optional<json> response;
    SessionDirective directive = SessionDirective::none;

    static auto error_response(const json& id, int code, const std::string& message) {
        return RequestResult{
            false,
            jsonrpc::create_error(id, code, message),
            SessionDirective::none
        };
    }

    static auto forward() {
        return RequestResult{
            true,
            std::nullopt,
            SessionDirective::none
        };
    }

    static auto local_response(const json& response, SessionDirective directive = SessionDirective::none) {
        return RequestResult{
            false,
            response,
            directive
        };
    }
};

struct SubscribeParam {
    std::string project_id;
    static std::optional<SubscribeParam> from_json(const json& params) {
        if (!params.is_object() || !params.contains("project_id") || !params["project_id"].is_string()) {
            return std::nullopt;
        }

        return SubscribeParam{params["project_id"].get<std::string>()};
    }
};

inline std::vector<std::string> workspace_folders_from_initialize_params(const json& params) {
    std::vector<std::string> folders;

    if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
        for (const auto& item : params["workspaceFolders"]) {
            if (item.is_object() && item.contains("uri") && item["uri"].is_string()) {
                folders.push_back(item["uri"].get<std::string>());
            }
        }
    }

    if (folders.empty() && params.contains("rootUri") && params["rootUri"].is_string()) {
        folders.push_back(params["rootUri"].get<std::string>());
    }

    if (folders.empty() && params.contains("rootPath") && params["rootPath"].is_string()) {
        folders.push_back(bridge_documents::file_uri_from_path(params["rootPath"].get<std::string>()));
    }

    return folders;
}

inline std::optional<bridge_documents::Range> range_from_json(const json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    const auto parse_position = [](const json& position) -> std::optional<bridge_documents::Position> {
        if (!position.is_object() || !position.contains("line") || !position.contains("character")) {
            return std::nullopt;
        }
        if (!position["line"].is_number_unsigned() || !position["character"].is_number_unsigned()) {
            return std::nullopt;
        }

        return bridge_documents::Position{
            position["line"].get<std::size_t>(),
            position["character"].get<std::size_t>()
        };
    };

    const auto start = parse_position(value.value("start", json::object()));
    const auto end = parse_position(value.value("end", json::object()));
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }

    return bridge_documents::Range{*start, *end};
}

inline std::vector<bridge_documents::TextChange> text_changes_from_json(const json& params) {
    std::vector<bridge_documents::TextChange> changes;
    if (!params.is_object() || !params.contains("contentChanges") || !params["contentChanges"].is_array()) {
        return changes;
    }

    for (const auto& item : params["contentChanges"]) {
        if (!item.is_object() || !item.contains("text") || !item["text"].is_string()) {
            continue;
        }

        changes.push_back(bridge_documents::TextChange{
            range_from_json(item.value("range", json())),
            bridge_documents::utf8_to_utf16(item["text"].get<std::string>())
        });
    }

    return changes;
}

inline std::string text_document_uri_from_params(const json& params) {
    if (params.contains("textDocument") && params["textDocument"].is_object()) {
        const auto& text_document = params["textDocument"];
        if (text_document.contains("uri") && text_document["uri"].is_string()) {
            return text_document["uri"].get<std::string>();
        }
    }

    if (params.contains("uri") && params["uri"].is_string()) {
        return params["uri"].get<std::string>();
    }

    return "";
}

inline json local_capabilities() {
    return json{
        {"capabilities", json{
            {"textDocumentSync", json{
                {"openClose", true},
                {"change", 2},
                {"save", json{{"includeText", false}}}
            }},
            {"workspace", json{{"workspaceFolders", json{{"supported", true}, {"changeNotifications", true}}}}}
        }}
    };
}

inline RequestResult handle_request(std::shared_ptr<LocalSession> session, const json& request, RequestContext* context = nullptr) {
    if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
        return RequestResult::error_response(request.value("id", nullptr), -32600, "Invalid Request: missing or invalid 'method' field");
    }

    const std::string method = request["method"].get<std::string>();
    const json params = request.value("params", json::object());
    if (method.rfind("simpletm/", 0) == 0) {
        if (method == "simpletm/subscribeProject") {
            const auto subscribe_params = SubscribeParam::from_json(params);
            if (!subscribe_params.has_value()) {
                return RequestResult::error_response(request.value("id", nullptr), -32602, "Invalid params");
            }

            session->register_subscription(subscribe_params->project_id);
            lsp::log_local_request(method, params);
        }
        return RequestResult::forward();
    }

    if (method == "textDocument/didOpen") {
        if (context != nullptr && context->document_manager) {
            const json text_document = params.value("textDocument", json::object());
            if (text_document.contains("uri") && text_document.contains("text")
                && text_document["uri"].is_string() && text_document["text"].is_string()) {
                context->document_manager->open_from_lsp(
                    text_document["uri"].get<std::string>(),
                    text_document["text"].get<std::string>(),
                    text_document.value("version", 0));
            }
        }
        lsp::log_local_notification(method, params);
        return {};
    }

    if (method == "textDocument/didChange") {
        if (context != nullptr && context->document_manager) {
            const std::string uri = text_document_uri_from_params(params);
            context->document_manager->apply_lsp_changes(
                uri,
                params.value("textDocument", json::object()).value("version", 0),
                text_changes_from_json(params));
        }
        lsp::log_local_notification(method, params);
        return {};
    }

    if (method == "textDocument/didSave") {
        if (context != nullptr && context->document_manager) {
            const std::string uri = text_document_uri_from_params(params);
            if (params.contains("text") && params["text"].is_string()) {
                context->document_manager->update_saved_document_text(uri, params["text"].get<std::string>());
            } else {
                context->document_manager->update_saved_document_from_disk(uri);
            }
        }
        lsp::log_local_notification(method, params);
        return {};
    }

    if (method == "textDocument/didClose") {
        if (context != nullptr && context->document_manager) {
            context->document_manager->close_document(text_document_uri_from_params(params));
        }
        lsp::log_local_notification(method, params);
        return {};
    }

    if (method == "workspace/didChangeWorkspaceFolders") {
        if (context != nullptr && context->document_manager && params.contains("event") && params["event"].is_object()) {
            const json event = params["event"];
            if (event.contains("added") && event["added"].is_array()) {
                for (const auto& item : event["added"]) {
                    if (item.is_object() && item.contains("uri") && item["uri"].is_string()) {
                        context->document_manager->add_workspace_folder(item["uri"].get<std::string>());
                    }
                }
            }

            if (event.contains("removed") && event["removed"].is_array()) {
                for (const auto& item : event["removed"]) {
                    if (item.is_object() && item.contains("uri") && item["uri"].is_string()) {
                        context->document_manager->remove_workspace_folder(item["uri"].get<std::string>());
                    }
                }
            }
        }
        lsp::log_local_notification(method, params);
        return {};
    }

    if (method == "initialized") {
        lsp::log_local_request(method, params);
        return {};
    }

    if (method == "$/setTrace") {
        lsp::log_local_request(method, params);
        return {};
    }

    if (method == "exit") {
        lsp::log_local_request(method, params);
        return RequestResult{false, std::nullopt, SessionDirective::close_session};
    }

    if (method == "dltxt/echo") {
        if (!request.contains("id")) {
            return {};
        }

        const std::string message = params.value("message", "");
        fprintf(stderr, "Echoing message from client: %s\n", message.c_str());

        return RequestResult::local_response(
            jsonrpc::create_response(request["id"], json{{"result", message}})
        );
    }

    if (method == "dltxt/version") {
        if (!request.contains("id")) {
            return {};
        }

        fprintf(stderr, "Responding to version request from client\n");
        return RequestResult::local_response(
            jsonrpc::create_response(request["id"], json{{"version", dltxt_bridge::version}})
        );
    }

    if (method == "dltxt/get_document_content") {
        if (!request.contains("id")) {
            return {};
        }

        if (!params.contains("uri") || !params["uri"].is_string()) {
            return RequestResult::error_response(request["id"], -32602, "Invalid params: missing or invalid 'uri' field");
        }

        json contents = json::object();
        if (context != nullptr && context->document_manager) {
            const std::string uri = params["uri"].get<std::string>();
            fprintf(stderr, "dltxt/get_document_content lookup uri=%s\n", uri.c_str());
            const auto content_opt = context->document_manager->document_content_utf8_by_uri(uri);
            if (content_opt.has_value()) {
                fprintf(stderr,
                    "dltxt/get_document_content hit uri=%s content_bytes=%zu\n",
                    uri.c_str(),
                    content_opt->size());
                contents["content"] = *content_opt;
            } else {
                fprintf(stderr,
                    "dltxt/get_document_content miss uri=%s open_document_count=%zu\n",
                    uri.c_str(),
                    context->document_manager->open_document_count());
                return RequestResult::error_response(request["id"], -32602, "Document not found for URI: " + uri);
            }
        }

        return RequestResult::local_response(
            jsonrpc::create_response(request["id"], std::move(contents))
        );
    }

    if (method == "dltxt/opened_documents") {
        if (!request.contains("id")) {
            return {};
        }

        json contents = json::object();
        if (context != nullptr && context->document_manager) {
            for (const auto& [uri, content] : context->document_manager->opened_document_contents_utf8_by_uri()) {
                contents[uri] = content;
            }
        }

        return RequestResult::local_response(
            jsonrpc::create_response(request["id"], std::move(contents))
        );
    }

    if (!request.contains("id")) {
        return {};
    }

    if (method == "initialize") {
        if (context != nullptr && context->document_manager) {
            context->document_manager->set_workspace_folders(workspace_folders_from_initialize_params(params));
        }
        fprintf(stderr, "Handled initialize request %s locally\n", request["id"].dump().c_str());
        return RequestResult::local_response(
            jsonrpc::create_response(request["id"], local_capabilities())
        );
    }

    if (method == "shutdown") {
        fprintf(stderr, "Handled shutdown request %s locally\n", request["id"].dump().c_str());
        return RequestResult::local_response(
            jsonrpc::create_response(request["id"], nullptr),
            SessionDirective::mark_shutdown_requested
        );
    }

    return RequestResult::error_response(request["id"], -32601, "method " + method + " is not recognized");
}
}

class ResponseManager {
private:
    struct Tracker {
        RequestId original_id;
        std::deque<json> responses;
        std::unique_ptr<net::steady_timer> signal;
    };
    std::map<RequestId, std::shared_ptr<Tracker>> pending_responses_;
    std::uint64_t next_bridge_request_id_ = 0;

public:
    enum class WaitStatus {
        response_ready,
        timed_out,
        cancelled,
    };

    struct WaitResult {
        WaitStatus status = WaitStatus::cancelled;
        std::optional<json> response;
    };

    RequestId create_bridge_request_id(
        const RequestId& original_id,
        net::any_io_executor ex,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) {

        if (!jsonrpc::is_trackable_request_id(original_id)) {
            throw std::invalid_argument("Null request IDs are not trackable");
        }

        RequestId bridge_id = std::string{"bridge-" + std::to_string(++next_bridge_request_id_)};
        auto tracker = std::make_shared<Tracker>();
        tracker->original_id = original_id;
        tracker->signal = std::make_unique<net::steady_timer>(ex, timeout);
        pending_responses_[bridge_id] = tracker;
        return bridge_id;
    }

    net::awaitable<WaitResult> wait_for_response(const RequestId& id) {
        auto it = pending_responses_.find(id);
        std::shared_ptr<Tracker> tracker = it != pending_responses_.end() ? it->second : nullptr;

        if (tracker) {
            if (!tracker->responses.empty()) {
                json response = std::move(tracker->responses.front());
                tracker->responses.pop_front();
                response["id"] = jsonrpc::request_id_to_json(tracker->original_id);
                co_return WaitResult{WaitStatus::response_ready, std::move(response)};
            }

            auto [error] = co_await tracker->signal->async_wait(net::as_tuple(net::use_awaitable));

            if (!tracker->responses.empty()) {
                json response = std::move(tracker->responses.front());
                tracker->responses.pop_front();
                response["id"] = jsonrpc::request_id_to_json(tracker->original_id);
                co_return WaitResult{WaitStatus::response_ready, std::move(response)};
            }

            if (!error) {
                co_return WaitResult{WaitStatus::timed_out, std::nullopt};
            }
        }

        co_return WaitResult{WaitStatus::cancelled, std::nullopt};
    }

    bool store_response(const RequestId& id, const json& response) {
        auto it = pending_responses_.find(id);
        if (it != pending_responses_.end()) {
            it->second->responses.push_back(response);
            it->second->signal->cancel();
            return true;
        }

        return false;
    }

    void cancel_all() {
        for (auto& [id, tracker] : pending_responses_) {
            if (tracker && tracker->signal) {
                tracker->signal->cancel();
            }
        }
    }

    void cleanup(const RequestId& id) {
        pending_responses_.erase(id);
    }
};

struct ResponseCleanup {
    std::shared_ptr<ResponseManager> manager;
    RequestId id;
    ~ResponseCleanup() { manager->cleanup(id); }
};