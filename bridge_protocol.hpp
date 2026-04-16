#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <array>
#include <algorithm>
#include <cctype>
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
    printf("%s (%zu bytes, escaped): [%s]\n", label, buffered.size(), escaped.c_str());
}

inline void append_chunk_to_buffer(beast::flat_buffer& buffer, const char* data, std::size_t size) {
    auto prepared = buffer.prepare(size);
    net::buffer_copy(prepared, net::buffer(data, size));
    buffer.commit(size);
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
        printf("%s\n", error.message().c_str());
        throw boost::system::system_error(error);
    }

    if (bytes_read == 0) {
        printf("Read returned 0 bytes\n");
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
            printf("Ignoring blank header block\n");
            continue;
        }

        auto content_length = parse_content_length(header_block);
        if (!content_length.has_value()) {
            printf("Invalid header block, unable to parse Content-Length\n");
            co_return std::nullopt;
        }

        printf("Parsed Content-Length: %zu\n", *content_length);

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
        printf("Received payload (%zu bytes):\n%s\n", payload.size(), payload.c_str());
        buffer.consume(*content_length);
        
        co_return payload;
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

inline json create_notification(const std::string& method, const json& params = json::object()) {
    return json{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
}
}

namespace bridge_local {
struct RequestHandlingResult {
    bool forward_to_remote = false;
    std::optional<json> response;
};

inline RequestHandlingResult handle_request(const json& request) {
    if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
        return {};
    }

    const std::string method = request["method"].get<std::string>();
    if (method.rfind("simpletm/", 0) == 0) {
        return RequestHandlingResult{true, std::nullopt};
    }

    if (!request.contains("id")) {
        return {};
    }

    if (method == "initialize") {
        printf("Handled initialize request %s locally\n", request["id"].dump().c_str());
        return RequestHandlingResult{
            false,
            jsonrpc::create_response(request["id"], json{{"capabilities", json::object()}})
        };
    }

    return RequestHandlingResult{
        false,
        jsonrpc::create_error(request["id"], -32601, "method " + method + " is not recognized")
    };
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

    void store_response(const RequestId& id, const json& response) {
        auto it = pending_responses_.find(id);
        if (it != pending_responses_.end()) {
            it->second->responses.push_back(response);
            it->second->signal->cancel();
        }
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