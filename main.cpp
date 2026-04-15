#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include "bridge_protocol.hpp"
#include "bridge_runtime.hpp"
#include <cstdio>
#include <string>
#include <memory>
#include <vector>
#include <deque>

namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;
namespace websocket = beast::websocket;
using RemoteRegistry = ActiveRemote<class RemoteClient>;

class LocalSessionManager {
private:
    std::vector<std::shared_ptr<tcp::socket>> sockets_;

public:
    void register_socket(std::shared_ptr<tcp::socket> socket) {
        sockets_.push_back(socket);
    }

    void unregister_socket(std::shared_ptr<tcp::socket> socket) {
        sockets_.erase(
            std::remove_if(sockets_.begin(), sockets_.end(),
                [socket](const std::shared_ptr<tcp::socket>& s) {
                    return s.get() == socket.get();
                }),
            sockets_.end()
        );
    }

    net::awaitable<void> broadcast(std::string message) {
        std::vector<std::shared_ptr<tcp::socket>> sockets_copy = sockets_;
        std::string framed_message = lsp::frame_message(message);
        
        for (auto& socket : sockets_copy) {
            if (socket && socket->is_open()) {
                try {
                    co_await net::async_write(*socket, net::buffer(framed_message), net::use_awaitable);
                } catch (...) {
                    // Socket may be closed, skip
                }
            }
        }
    }
};

class RemoteClient {
    net::strand<net::any_io_executor> strand_;
    websocket::stream<beast::tcp_stream> ws_;
    std::deque<std::string> write_queue_;
    bool write_in_progress_ = false;

public:
    explicit RemoteClient(net::any_io_executor ex)
        : strand_(net::make_strand(ex)),
          ws_(strand_) {}

    net::awaitable<void> connect(std::string host, std::string port) {
        co_await net::dispatch(strand_, net::use_awaitable);

        tcp::resolver resolver(strand_);
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
        co_await net::dispatch(strand_, net::use_awaitable);

        write_queue_.push_back(std::move(message));
        if (write_in_progress_) {
            co_return;
        }

        write_in_progress_ = true;

        try {
            while (!write_queue_.empty()) {
                std::string next = std::move(write_queue_.front());
                write_queue_.pop_front();

                printf("Sending to Go backend: %s\n", next.c_str());
                co_await ws_.async_write(net::buffer(next), net::use_awaitable);
                printf("Message sent to Go backend\n");
            }
        } catch (...) {
            write_in_progress_ = false;
            throw;
        }

        write_in_progress_ = false;
    }

    net::awaitable<std::string> receive() {
        co_await net::dispatch(strand_, net::use_awaitable);

        beast::flat_buffer buffer;
        co_await ws_.async_read(buffer, net::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    }
};

net::awaitable<void> handle_local_session(
    tcp::socket socket, 
    std::shared_ptr<RemoteRegistry> remote_registry,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager) {
    
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    session_manager->register_socket(socket_ptr);
    
    beast::flat_buffer buffer;
    
    try {
        for (;;) {
            auto maybe_message = co_await lsp::read_message(*socket_ptr, buffer);
            if (!maybe_message.has_value()) {
                fprintf(stderr, "Invalid LSP header received\n");
                continue;
            }

            std::string msg_str = std::move(*maybe_message);
            if (msg_str.empty()) {
                continue;
            }
            
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
                    std::string framed = lsp::frame_message(error_resp.dump());
                    co_await net::async_write(*socket_ptr, net::buffer(framed), net::use_awaitable);
                }
                continue;
            }
            
            if (!request.contains("method")) {
                if (request.contains("id")) {
                    json error_resp = jsonrpc::create_error(request["id"], -32600, "Missing method");
                    std::string framed = lsp::frame_message(error_resp.dump());
                    co_await net::async_write(*socket_ptr, net::buffer(framed), net::use_awaitable);
                }
                continue;
            }
            
            printf("JSON-RPC Request: %s\n", request.dump().c_str());
            
            auto remote = remote_registry->get();
            if (!remote) {
                if (request.contains("id")) {
                    json error_resp = jsonrpc::create_error(request["id"], -32000, "Remote server unavailable");
                    std::string framed = lsp::frame_message(error_resp.dump());
                    co_await net::async_write(*socket_ptr, net::buffer(framed), net::use_awaitable);
                }
                continue;
            }
            
            if (request.contains("id")) {
                auto maybe_id = jsonrpc::request_id_from_json(request["id"]);
                if (!maybe_id.has_value()) {
                    json error_resp = jsonrpc::create_error(request["id"], -32600, "Invalid Request id");
                    std::string framed = lsp::frame_message(error_resp.dump());
                    co_await net::async_write(*socket_ptr, net::buffer(framed), net::use_awaitable);
                    continue;
                }

                RequestId id = *maybe_id;
                auto executor = co_await net::this_coro::executor;
                response_manager->create_request_tracker(id, executor);

                // RAII Guard: Automatically calls cleanup(id) when this object goes out of scope
                ResponseCleanup guard{response_manager, id};

                // Forward request to Go
                co_await remote->send(request.dump());

                auto maybe_response = co_await response_manager->wait_for_response(id);

                if (maybe_response.has_value()) {
                    std::string out = lsp::frame_message(maybe_response->dump());
                    co_await net::async_write(
                        *socket_ptr, 
                        net::buffer(out), 
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
                    auto maybe_id = jsonrpc::request_id_from_json(response["id"]);
                    if (maybe_id.has_value()) {
                        response_manager->store_response(*maybe_id, response);
                    }
                } else {
                    // Otherwise broadcast as notification to all local sessions
                    co_await session_manager->broadcast(message);
                }
            } else {
                // If not JSON, print error and move on
                fprintf(stderr, "Received non-JSON message from Go backend: %s\n", message.c_str());
            }
        }
    } catch (std::exception& e) {
        fprintf(stderr, "Remote reader error: %s\n", e.what());
        throw;
    }
}
// Coroutine to listen for new VS Code windows
net::awaitable<void> listener(
    unsigned short port, 
    std::shared_ptr<RemoteRegistry> remote_registry,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager) {
    auto executor = co_await net::this_coro::executor;
    tcp::acceptor acceptor(executor, {tcp::v4(), port});
    
    printf("dltxt_bridge (Coroutine mode) on port %u - JSON-RPC Language Server\n", port);

    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);

        net::co_spawn(socket.get_executor(), 
            handle_local_session(std::move(socket), remote_registry, session_manager, response_manager), 
            net::detached);
    }
}

int main() {
    try {
        net::io_context ioc;

        auto remote_registry = std::make_shared<RemoteRegistry>();
        auto session_manager = std::make_shared<LocalSessionManager>();
        auto response_manager = std::make_shared<ResponseManager>();

        // Launch ONE coordinator coroutine to handle startup order
        net::co_spawn(ioc, [remote_registry, session_manager, response_manager]() mutable -> net::awaitable<void> {
            auto ex = co_await net::this_coro::executor;
            net::co_spawn(ex, listener(6009, remote_registry, session_manager, response_manager), net::detached);

            co_await run_remote_retry_loop(
                remote_registry,
                [](net::any_io_executor executor) {
                    return std::make_shared<RemoteClient>(executor);
                },
                [session_manager, response_manager](const std::shared_ptr<RemoteClient>& remote_client_ptr) -> net::awaitable<void> {
                    try {
                        // WAIT for the connection to be fully established
                        co_await remote_client_ptr->connect("127.0.0.1", "9000");

                        // Start the loops that depend on that connection
                        co_await remote_reader(remote_client_ptr, session_manager, response_manager);
                    } catch (...) {
                        response_manager->cancel_all();
                        throw;
                    }

                    response_manager->cancel_all();
                },
                [](std::exception_ptr error) {
                    try {
                        if (error) {
                            std::rethrow_exception(error);
                        }
                    } catch (const std::exception& e) {
                        fprintf(stderr, "Startup or Connection Error: %s\n", e.what());
                    } catch (...) {
                        fprintf(stderr, "Startup or Connection Error: unknown exception\n");
                    }
                }
            );
            
            co_return;
        }, net::detached);

        ioc.run();
    } catch (std::exception& e) {
        fprintf(stderr, "Bridge Error: %s\n", e.what());
        return 1;
    }
    return 0;
}