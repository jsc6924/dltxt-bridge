#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace net = boost::asio;
namespace beast = boost::beast;
using json = nlohmann::json;

namespace lsp {
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
                return static_cast<std::size_t>(std::stoull(value));
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
    std::map<json, std::shared_ptr<Tracker>> pending_responses_;

public:
    std::shared_ptr<Tracker> create_request_tracker(const json& id, net::any_io_executor ex) {
        auto tracker = std::make_shared<Tracker>();
        tracker->signal = std::make_unique<net::steady_timer>(ex, std::chrono::steady_clock::time_point::max());
        pending_responses_[id] = tracker;
        return tracker;
    }

    net::awaitable<std::optional<json>> wait_for_response(json id) {
        auto it = pending_responses_.find(id);
        std::shared_ptr<Tracker> tracker = it != pending_responses_.end() ? it->second : nullptr;

        if (tracker) {
            co_await tracker->signal->async_wait(net::as_tuple(net::use_awaitable));

            if (!tracker->responses.empty()) {
                json response = std::move(tracker->responses.front());
                tracker->responses.pop_front();
                co_return response;
            }
        }
        co_return std::nullopt;
    }

    void store_response(const json& id, const json& response) {
        auto it = pending_responses_.find(id);
        if (it != pending_responses_.end()) {
            it->second->responses.push_back(response);
            it->second->signal->cancel();
        }
    }

    void cleanup(const json& id) {
        pending_responses_.erase(id);
    }
};

struct ResponseCleanup {
    std::shared_ptr<ResponseManager> manager;
    json id;
    ~ResponseCleanup() { manager->cleanup(id); }
};