#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <map>
#include <deque>
#include <condition_variable>

namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;
using json = nlohmann::json;
namespace websocket = beast::websocket;

// JSON-RPC utilities
namespace jsonrpc {
    json create_response(const json& id, const json& result) {
        return json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", result}
        };
    }

    json create_error(const json& id, int code, const std::string& message) {
        return json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", json{{"code", code}, {"message", message}}}
        };
    }

    json create_notification(const std::string& method, const json& params = json::object()) {
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
    std::mutex mutex_;

public:
    std::shared_ptr<Tracker> create_request_tracker(const json& id, net::any_io_executor ex) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto tracker = std::make_shared<Tracker>();
        // Create a timer that essentially never expires
        tracker->signal = std::make_unique<net::steady_timer>(ex, std::chrono::steady_clock::time_point::max());
        pending_responses_[id] = tracker;
        return tracker;
    }

    // New helper to hide the "ugly" timer logic
    net::awaitable<std::optional<json>> wait_for_response(json id) {
        std::shared_ptr<Tracker> tracker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tracker = pending_responses_[id];
        }

        if (tracker) {
            // Wait without throwing exceptions
            co_await tracker->signal->async_wait(net::as_tuple(net::use_awaitable));
            
            if (!tracker->responses.empty()) {
                co_return std::move(tracker->responses.front());
            }
        }
        co_return std::nullopt; // Or handle timeout/error
    }

    void store_response(const json& id, const json& response) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_responses_.find(id);
        if (it != pending_responses_.end()) {
            it->second->responses.push_back(response);
            // This is the "Signal": cancelling the timer wakes up the co_await
            it->second->signal->cancel();
        }
    }

    void cleanup(const json& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_responses_.erase(id);
    }
};

struct ResponseCleanup {
    std::shared_ptr<ResponseManager> manager;
    json id;
    ~ResponseCleanup() { manager->cleanup(id); }
};

class LocalSessionManager {
private:
    std::vector<std::shared_ptr<tcp::socket>> sockets_;
    std::mutex mutex_;

public:
    void register_socket(std::shared_ptr<tcp::socket> socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_.push_back(socket);
    }

    void unregister_socket(std::shared_ptr<tcp::socket> socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_.erase(
            std::remove_if(sockets_.begin(), sockets_.end(),
                [socket](const std::shared_ptr<tcp::socket>& s) {
                    return s.get() == socket.get();
                }),
            sockets_.end()
        );
    }

    net::awaitable<void> broadcast(std::string message) {
        std::vector<std::shared_ptr<tcp::socket>> sockets_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sockets_copy = sockets_;
        }
        
        for (auto& socket : sockets_copy) {
            if (socket && socket->is_open()) {
                try {
                    co_await socket->async_write_some(
                        net::buffer(message),
                        net::use_awaitable
                    );
                } catch (...) {
                    // Socket may be closed, skip
                }
            }
        }
    }
};

class RemoteClient {
    websocket::stream<beast::tcp_stream> ws_;
    std::mutex ws_mutex_;  // Protect concurrent access to WebSocket

public:
    explicit RemoteClient(net::any_io_executor ex) : ws_(ex) {}

    net::awaitable<void> connect(std::string host, std::string port) {
        tcp::resolver resolver(ws_.get_executor());
        auto const results = co_await resolver.async_resolve(host, port, net::use_awaitable);

        // Connect and Handshake
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        co_await beast::get_lowest_layer(ws_).async_connect(results, net::use_awaitable);
        
        // Turn off timeout on the tcp_stream because websocket has its own
        beast::get_lowest_layer(ws_).expires_never();

        co_await ws_.async_handshake(host, "/ws", net::use_awaitable);
        printf("Connected to Go Backend at %s:%s\n", host.c_str(), port.c_str());
    }

    net::awaitable<void> send(std::string message) {
        printf("Sending to Go backend: %s\n", message.c_str());
        co_await ws_.async_write(net::buffer(message), net::use_awaitable);
        printf("Message sent to Go backend\n");
    }

    net::awaitable<std::string> receive() {
        beast::flat_buffer buffer;
        co_await ws_.async_read(buffer, net::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    }
};

net::awaitable<void> handle_local_session(
    tcp::socket socket, 
    std::shared_ptr<RemoteClient> remote,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager) {
    
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    session_manager->register_socket(socket_ptr);
    
    beast::flat_buffer buffer;
    
    try {
        for (;;) {
            // 1. Read until newline
            std::size_t n = co_await net::async_read_until(*socket_ptr, buffer, '\n', net::use_awaitable);
            
            // 2. Extract ONLY the message part
            auto data = buffer.data();
            std::string msg_str = beast::buffers_to_string(beast::buffers_prefix(n, data));
            // 3. Consume only what we processed (preserving the rest!)
            buffer.consume(n);
            if (!msg_str.empty() && msg_str.back() == '\n') {
                msg_str.pop_back();
            }
            if (msg_str.empty()) continue;
            
            // Parse JSON-RPC
            bool parse_error = false;
            json request;
            
            try {
                request = json::parse(msg_str);
            } catch (const std::exception& e) {
                fprintf(stderr, "JSON parse error: %s\n", e.what());
                parse_error = true;
            }
            
            if (parse_error) continue;
            
            // Validate JSON-RPC request
            if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
                if (request.contains("id")) {
                    json error_resp = jsonrpc::create_error(request["id"], -32600, "Invalid Request");
                    co_await socket_ptr->async_write_some(
                        net::buffer(error_resp.dump() + "\n"), 
                        net::use_awaitable);
                }
                continue;
            }
            
            if (!request.contains("method")) {
                if (request.contains("id")) {
                    json error_resp = jsonrpc::create_error(request["id"], -32600, "Missing method");
                    co_await socket_ptr->async_write_some(
                        net::buffer(error_resp.dump() + "\n"), 
                        net::use_awaitable);
                }
                continue;
            }
            
            printf("JSON-RPC Request: %s\n", request.dump().c_str());
            
            // Forward to Go backend
            co_await remote->send(request.dump());
            
            if (request.contains("id")) {
                auto id = request["id"];
                auto executor = co_await net::this_coro::executor;
                auto tracker = response_manager->create_request_tracker(id, executor);

                // RAII Guard: Automatically calls cleanup(id) when this object goes out of scope
                ResponseCleanup guard{response_manager, id};

                // Forward request to Go
                co_await remote->send(request.dump());

                auto maybe_response = co_await response_manager->wait_for_response(id);

                if (maybe_response.has_value()) {
                    json real_response = maybe_response.value();
                    co_await socket_ptr->async_write_some(
                        net::buffer(real_response.dump() + "\n"), 
                        net::use_awaitable
                    );
                }
            } else {
                // It's a notification, just fire and forget
                co_await remote->send(request.dump());
            }
        }
    } catch (...) { 
        /* Handle disconnect */ 
        session_manager->unregister_socket(socket_ptr);
    }
}
// Remote reader coroutine: continuously receives from Go backend and broadcasts to all local sessions
net::awaitable<void> remote_reader(
    std::shared_ptr<RemoteClient> remote,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager) {
    
    try {
        for (;;) {
            std::string message = co_await remote->receive();
            printf("Received from Go backend: %s\n", message.c_str());
            
            // Parse as JSON-RPC if possible
            bool is_json = false;
            json response;
            
            try {
                response = json::parse(message);
                is_json = true;
            } catch (...) {
                is_json = false;
            }
            
            if (is_json) {
                // If it has an id, it's a response to a specific request
                if (response.contains("id")) {
                    response_manager->store_response(response["id"], response);
                } else {
                    // Otherwise broadcast as notification to all local sessions
                    co_await session_manager->broadcast(message + "\n");
                }
            } else {
                // If not JSON, just broadcast
                co_await session_manager->broadcast(message + "\n");
            }
        }
    } catch (std::exception& e) {
        fprintf(stderr, "Remote reader error: %s\n", e.what());
    }
}
// Coroutine to listen for new VS Code windows
net::awaitable<void> listener(
    unsigned short port, 
    std::shared_ptr<RemoteClient> remote_client_ptr,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager) {
    auto executor = co_await net::this_coro::executor;
    tcp::acceptor acceptor(executor, {tcp::v4(), port});
    
    printf("dltxt_bridge (Coroutine mode) on port %u - JSON-RPC Language Server\n", port);

    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);

        net::co_spawn(socket.get_executor(), 
            handle_local_session(std::move(socket), remote_client_ptr, session_manager, response_manager), 
            net::detached);
    }
}

int main() {
    try {
        net::io_context ioc;

        auto remote_client_ptr = std::make_shared<RemoteClient>(ioc.get_executor());
        auto session_manager = std::make_shared<LocalSessionManager>();
        auto response_manager = std::make_shared<ResponseManager>();

        // Launch ONE coordinator coroutine to handle startup order
        net::co_spawn(ioc, [remote_client_ptr, session_manager, response_manager]() mutable -> net::awaitable<void> {
            auto ex = co_await net::this_coro::executor;
            net::co_spawn(ex, listener(6009, remote_client_ptr, session_manager, response_manager), net::detached);

            for(;;) {
                try {
                    // WAIT for the connection to be fully established
                    co_await remote_client_ptr->connect("127.0.0.1", "9000");

                    // Start the loops that depend on that connection
                    co_await remote_reader(remote_client_ptr, session_manager, response_manager);
                } catch (std::exception& e) {
                    fprintf(stderr, "Startup or Connection Error: %s\n", e.what());
                }
            }
            
            co_return;
        }, net::detached);

        ioc.run();
    } catch (std::exception& e) {
        fprintf(stderr, "Bridge Error: %s\n", e.what());
        return 1;
    }
    return 0;
}