#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include "bridge_app.hpp"
#include "bridge_protocol.hpp"
#include "bridge_runtime.hpp"
#include <cstdio>
#include <exception>
#include <memory>
#include <deque>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using RemoteRegistry = ActiveRemote<class RemoteClient>;
net::awaitable<void> write_json_message(const std::shared_ptr<LocalSession>& session, const json& message);
net::awaitable<void> write_json_error(const std::shared_ptr<LocalSession>& session, const json& id, int code, const std::string& message);

void configure_stdio_for_immediate_flush() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

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

net::awaitable<void> heartbeat_loop(std::shared_ptr<LocalSession> session) {
    try {
        auto socket = session->get_socket();
        while (session->is_open()) {
            net::steady_timer timer(socket->get_executor());
            timer.expires_after(std::chrono::seconds(10));
            co_await timer.async_wait(net::use_awaitable);

            if (!session->is_open()) {
                break;
            }

            co_await write_json_message(session, jsonrpc::create_notification("dltxt/notification", {{"message", "heartbeat"}}));
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Heartbeat loop error: %s\n", e.what());
    }
}

net::awaitable<void> write_json_message(const std::shared_ptr<LocalSession>& session, const json& message) {
    printf("[response] %s\n", message.dump().c_str());
    std::string framed = lsp::frame_message(message.dump());
    co_await session->write_framed(std::move(framed));
}

net::awaitable<void> write_json_error(const std::shared_ptr<LocalSession>& session, const json& id, int code, const std::string& message) {
    co_await write_json_message(session, jsonrpc::create_error(id, code, message));
}

net::awaitable<void> handle_local_session(
    tcp::socket socket, 
    std::shared_ptr<RemoteRegistry> remote_registry,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager,
    std::chrono::milliseconds request_timeout) {
    
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    boost::system::error_code endpoint_error;
    const auto remote_endpoint = socket_ptr->remote_endpoint(endpoint_error);
    auto session = session_manager->register_socket(socket_ptr);

    if (!endpoint_error) {
        printf("Accepted local client %s:%u\n", remote_endpoint.address().to_string().c_str(), remote_endpoint.port());
    } else {
        fprintf(stderr, "Accepted local client with unknown endpoint: %s\n", endpoint_error.message().c_str());
    }
    
    beast::flat_buffer buffer;
    bool shutdown_requested = false;

    net::co_spawn(socket_ptr->get_executor(), heartbeat_loop(session), net::detached);
    
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
                    co_await write_json_error(session, request["id"], -32600, "Invalid Request");
                }
                continue;
            }
            
            if (!request.contains("method")) {
                if (request.contains("id")) {
                    co_await write_json_error(session, request["id"], -32600, "Missing method");
                }
                continue;
            }
            
            printf("JSON-RPC Request: %s\n", request.dump().c_str());

            auto local_handling = bridge_local::handle_request(session, request);
            if (!local_handling.forward_to_remote) {
                if (local_handling.response.has_value()) {
                    co_await write_json_message(session, *local_handling.response);
                }

                if (local_handling.directive == bridge_local::SessionDirective::mark_shutdown_requested) {
                    shutdown_requested = true;
                } else if (local_handling.directive == bridge_local::SessionDirective::close_session) {
                    printf(
                        shutdown_requested
                            ? "Closing local session after exit notification following shutdown\n"
                            : "Closing local session after exit notification without prior shutdown\n"
                    );
                    close_socket_if_open(socket_ptr);
                    session_manager->unregister(session);
                    co_return;
                }

                continue;
            }
            
            auto remote = remote_registry->get();
            if (!remote) {
                if (request.contains("id")) {
                    co_await write_json_error(session, request["id"], -32000, "Remote server unavailable");
                }
                continue;
            }
            
            if (request.contains("id")) {
                auto maybe_id = jsonrpc::request_id_from_json(request["id"]);
                if (!maybe_id.has_value() || !jsonrpc::is_trackable_request_id(*maybe_id)) {
                    co_await write_json_error(session, request["id"], -32600, "Invalid Request id");
                    continue;
                }

                RequestId original_id = *maybe_id;
                json original_id_json = request["id"];
                auto executor = co_await net::this_coro::executor;
                RequestId bridge_id = response_manager->create_bridge_request_id(original_id, executor, request_timeout);
                request["id"] = jsonrpc::request_id_to_json(bridge_id);

                // RAII Guard: Automatically calls cleanup(id) when this object goes out of scope
                ResponseCleanup guard{response_manager, bridge_id};

                // Forward request to Go
                co_await remote->send(request.dump());

                auto wait_result = co_await response_manager->wait_for_response(bridge_id);

                if (wait_result.status == ResponseManager::WaitStatus::response_ready && wait_result.response.has_value()) {
                    co_await write_json_message(session, *wait_result.response);
                } else if (wait_result.status == ResponseManager::WaitStatus::timed_out) {
                    co_await write_json_error(session, original_id_json, -32001, "Request timed out");
                } else if (wait_result.status == ResponseManager::WaitStatus::cancelled) {
                    co_await write_json_error(session, original_id_json, -32002, "Remote server disconnected while awaiting response");
                }
            } else {
                // It's a notification, just fire and forget
                co_await remote->send(request.dump());
            }
        }
    } catch (...) { 
        /* Handle disconnect */ 
        if (!endpoint_error) {
            printf("Closing local client %s:%u\n", remote_endpoint.address().to_string().c_str(), remote_endpoint.port());
        }
        close_socket_if_open(socket_ptr);
        session_manager->unregister(session);
    }
}
// Remote reader coroutine: continuously receives from Go backend and broadcasts to all local sessions
net::awaitable<void> remote_reader(
    std::shared_ptr<RemoteClient> remote,
    std::shared_ptr<ResponseManager> response_manager,
    std::shared_ptr<RemoteNotificationQueue> notification_queue) {
    
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
                if (jsonrpc::is_valid_jsonrpc_response(response)) {
                    auto maybe_id = jsonrpc::request_id_from_json(response["id"]);
                    if (maybe_id.has_value() && jsonrpc::is_trackable_request_id(*maybe_id)) {
                        response_manager->store_response(*maybe_id, response);
                    }
                } else if (jsonrpc::is_valid_jsonrpc_notification(response)) {
                    notification_queue->push(std::move(message));
                } else {
                    fprintf(stderr, "Invalid JSON-RPC message from Go backend ignored: %s\n", message.c_str());
                }
            } else {
                fprintf(stderr, "Non-JSON backend message ignored: %s\n", message.c_str());
            }
        }
    } catch (std::exception& e) {
        notification_queue->close();
        fprintf(stderr, "Remote reader error: %s\n", e.what());
        throw;
    }
}
// Coroutine to listen for new VS Code windows
net::awaitable<void> listener(
    unsigned short port, 
    std::shared_ptr<RemoteRegistry> remote_registry,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager,
    std::chrono::milliseconds request_timeout) {
    auto executor = co_await net::this_coro::executor;
    tcp::acceptor acceptor(executor, {tcp::v4(), port});
    
    printf("dltxt_bridge (Coroutine mode) on port %u - JSON-RPC Language Server\n", port);

    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);

        net::co_spawn(socket.get_executor(), 
            handle_local_session(std::move(socket), remote_registry, session_manager, response_manager, request_timeout), 
            net::detached);
    }
}

int main(int argc, char* argv[]) {
    try {
        configure_stdio_for_immediate_flush();
        const BridgeSettings settings = parse_bridge_settings(argc, argv);
        net::io_context ioc;
        auto stop_signals = install_stop_signals(ioc);

        auto remote_registry = std::make_shared<RemoteRegistry>();
        auto session_manager = std::make_shared<LocalSessionManager>();
        auto response_manager = std::make_shared<ResponseManager>();

        // Launch ONE coordinator coroutine to handle startup order
        net::co_spawn(ioc, [remote_registry, session_manager, response_manager, settings]() mutable -> net::awaitable<void> {
            auto ex = co_await net::this_coro::executor;
            net::co_spawn(ex, listener(settings.local_port, remote_registry, session_manager, response_manager, settings.request_timeout), net::detached);

            co_await run_remote_retry_loop(
                remote_registry,
                [](net::any_io_executor executor) {
                    return std::make_shared<RemoteClient>(executor);
                },
                [session_manager, response_manager, settings](const std::shared_ptr<RemoteClient>& remote_client_ptr) -> net::awaitable<void> {
                    auto executor = co_await net::this_coro::executor;
                    auto notification_queue = std::make_shared<RemoteNotificationQueue>(executor);
                    net::co_spawn(
                        executor,
                        remote_notification_forwarder(notification_queue, session_manager),
                        net::detached
                    );

                    try {
                        // WAIT for the connection to be fully established
                        co_await remote_client_ptr->connect(settings.remote_host, settings.remote_port);

                        // Start the loops that depend on that connection
                        co_await remote_reader(remote_client_ptr, response_manager, notification_queue);
                    } catch (...) {
                        notification_queue->close();
                        response_manager->cancel_all();
                        throw;
                    }

                    notification_queue->close();
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
    } catch (const std::invalid_argument& e) {
        fprintf(stderr, "Configuration Error: %s\n", e.what());
        fprintf(stderr, "Usage: dltxt_bridge [--listen-port <port>] [--remote-host <host>] [--remote-port <port>] [--request-timeout-ms <ms>]\n");
        return 1;
    } catch (std::exception& e) {
        fprintf(stderr, "Bridge Error: %s\n", e.what());
        return 1;
    }
    return 0;
}