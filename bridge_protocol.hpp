#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <algorithm>
#include <cctype>
#include <limits>
#include <deque>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
    std::size_t header_bytes = co_await net::async_read_until(stream, buffer, "\r\n\r\n", net::use_awaitable);

    std::string header_block = beast::buffers_to_string(beast::buffers_prefix(header_bytes, buffer.data()));
    buffer.consume(header_bytes);

    auto content_length = parse_content_length(header_block);
    if (!content_length.has_value()) {
        co_return std::nullopt;
    }

    while (buffer.size() < *content_length) {
        co_await net::async_read(stream, buffer, net::transfer_at_least(*content_length - buffer.size()), net::use_awaitable);
    }

    std::string payload = beast::buffers_to_string(beast::buffers_prefix(*content_length, buffer.data()));
    buffer.consume(*content_length);
    co_return payload;
}
}

namespace jsonrpc {
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

class ResponseManager {
private:
    struct Tracker {
        std::deque<json> responses;
        std::unique_ptr<net::steady_timer> signal;
    };
    std::map<RequestId, std::shared_ptr<Tracker>> pending_responses_;

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

    std::shared_ptr<Tracker> create_request_tracker(
        const RequestId& id,
        net::any_io_executor ex,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) {

        auto tracker = std::make_shared<Tracker>();
        tracker->signal = std::make_unique<net::steady_timer>(ex, timeout);
        pending_responses_[id] = tracker;
        return tracker;
    }

    net::awaitable<WaitResult> wait_for_response(const RequestId& id) {
        auto it = pending_responses_.find(id);
        std::shared_ptr<Tracker> tracker = it != pending_responses_.end() ? it->second : nullptr;

        if (tracker) {
            auto [error] = co_await tracker->signal->async_wait(net::as_tuple(net::use_awaitable));

            if (!tracker->responses.empty()) {
                json response = std::move(tracker->responses.front());
                tracker->responses.pop_front();
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