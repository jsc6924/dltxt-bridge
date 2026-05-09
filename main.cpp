#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include "bridge_app.hpp"
#include "bridge_documents.hpp"
#include "bridge_http_proxy.hpp"
#include "bridge_protocol.hpp"
#include "bridge_runtime.hpp"
#include "bridge_version.hpp"
#include <cstdio>
#include <exception>
#include <memory>
#include <deque>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = net::ssl;
using RemoteRegistry = ActiveRemote<class RemoteClient>;
net::awaitable<void> write_json_message(const std::shared_ptr<LocalSession>& session, const json& message);
net::awaitable<void> write_json_error(const std::shared_ptr<LocalSession>& session, const json& id, int code, const std::string& message);

void configure_stdio_for_immediate_flush() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

class RemoteClient {
    net::strand<net::any_io_executor> strand_;
    std::optional<ssl::context> ssl_ctx_;
    using PlainWs = websocket::stream<beast::tcp_stream>;
    using SslWs   = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
    std::variant<PlainWs, SslWs> ws_;
    std::deque<std::string> write_queue_;
    bool write_in_progress_ = false;

public:
    explicit RemoteClient(net::any_io_executor ex, bool use_ssl)
        : strand_(net::make_strand(ex)),
          ssl_ctx_(use_ssl ? std::optional<ssl::context>(ssl::context::tls_client) : std::nullopt),
          ws_(use_ssl
              ? std::variant<PlainWs, SslWs>(std::in_place_type<SslWs>, strand_, *ssl_ctx_)
              : std::variant<PlainWs, SslWs>(std::in_place_type<PlainWs>, strand_)) {
        if (ssl_ctx_) {
            ssl_ctx_->set_default_verify_paths();
            ssl_ctx_->set_verify_mode(ssl::verify_peer);
        }
    }

    net::awaitable<void> connect(std::string host, std::string port) {
        co_await net::dispatch(strand_, net::use_awaitable);

        tcp::resolver resolver(strand_);
        auto const results = co_await resolver.async_resolve(host, port, net::use_awaitable);

        if (auto* plain = std::get_if<PlainWs>(&ws_)) {
            beast::get_lowest_layer(*plain).expires_after(std::chrono::seconds(30));
            co_await beast::get_lowest_layer(*plain).async_connect(results, net::use_awaitable);
            beast::get_lowest_layer(*plain).expires_never();
            co_await plain->async_handshake(host, "/ws", net::use_awaitable);
        } else {
            auto& ssl_ws = std::get<SslWs>(ws_);
            beast::get_lowest_layer(ssl_ws).expires_after(std::chrono::seconds(30));
            co_await beast::get_lowest_layer(ssl_ws).async_connect(results, net::use_awaitable);

            // Set SNI hostname - required for virtual hosting / Let's Encrypt
            if (!SSL_set_tlsext_host_name(ssl_ws.next_layer().native_handle(), host.c_str())) {
                beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
                throw beast::system_error{ec};
            }

            beast::get_lowest_layer(ssl_ws).expires_after(std::chrono::seconds(30));
            co_await ssl_ws.next_layer().async_handshake(ssl::stream_base::client, net::use_awaitable);
            beast::get_lowest_layer(ssl_ws).expires_never();
            co_await ssl_ws.async_handshake(host, "/ws", net::use_awaitable);
        }

        fprintf(stderr, "Connected to Go Backend at %s:%s\n", host.c_str(), port.c_str());
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

                fprintf(stderr, "Sending to Go backend: %s\n", next.c_str());
                if (auto* plain = std::get_if<PlainWs>(&ws_)) {
                    co_await plain->async_write(net::buffer(next), net::use_awaitable);
                } else {
                    co_await std::get<SslWs>(ws_).async_write(net::buffer(next), net::use_awaitable);
                }
                fprintf(stderr, "Message sent to Go backend\n");
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
        if (auto* plain = std::get_if<PlainWs>(&ws_)) {
            co_await plain->async_read(buffer, net::use_awaitable);
        } else {
            co_await std::get<SslWs>(ws_).async_read(buffer, net::use_awaitable);
        }
        co_return beast::buffers_to_string(buffer.data());
    }
};

net::awaitable<void> heartbeat_loop(std::shared_ptr<LocalSession> session, std::shared_ptr<RemoteRegistry> remote_registry) {
    try {
        while (session->is_open()) {
            net::steady_timer timer(session->get_executor());
            timer.expires_after(std::chrono::seconds(10));
            co_await timer.async_wait(net::use_awaitable);

            if (!session->is_open()) {
                break;
            }

            co_await write_json_message(
                session,
                jsonrpc::create_notification(
                    "dltxt/notification",
                    json{{"message", "heartbeat"}, {"remote_available", remote_registry->is_active()}}
                )
            );
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Heartbeat loop error: %s\n", e.what());
    }
}

net::awaitable<void> write_json_message(const std::shared_ptr<LocalSession>& session, const json& message) {
    std::string framed = lsp::frame_message(message.dump());
    co_await session->write_framed(std::move(framed));
}

net::awaitable<void> write_json_error(const std::shared_ptr<LocalSession>& session, const json& id, int code, const std::string& message) {
    co_await write_json_message(session, jsonrpc::create_error(id, code, message));
}

class LocalInputQueue {
    net::steady_timer signal_;
    std::deque<std::string> messages_;
    std::exception_ptr error_;
    bool closed_ = false;

public:
    explicit LocalInputQueue(net::any_io_executor executor)
        : signal_(executor) {
        signal_.expires_at((std::chrono::steady_clock::time_point::max)());
    }

    void push(std::string message) {
        if (closed_) {
            return;
        }

        messages_.push_back(std::move(message));
        signal_.cancel_one();
    }

    void close() {
        if (closed_) {
            return;
        }

        closed_ = true;
        signal_.cancel();
    }

    void fail(std::exception_ptr error) {
        error_ = error;
        closed_ = true;
        signal_.cancel();
    }

    net::awaitable<std::optional<std::string>> wait_and_pop() {
        for (;;) {
            if (!messages_.empty()) {
                std::string message = std::move(messages_.front());
                messages_.pop_front();
                co_return message;
            }

            if (error_) {
                std::rethrow_exception(error_);
            }

            if (closed_) {
                co_return std::nullopt;
            }

            signal_.expires_at((std::chrono::steady_clock::time_point::max)());
            auto [error] = co_await signal_.async_wait(net::as_tuple(net::use_awaitable));
            if (error && error != net::error::operation_aborted) {
                throw boost::system::system_error(error);
            }
        }
    }
};

void start_stdio_reader_thread(
    net::any_io_executor executor,
    std::shared_ptr<StdioHandle> input_handle,
    std::shared_ptr<LocalInputQueue> input_queue) {

    std::thread([executor, input_handle = std::move(input_handle), input_queue = std::move(input_queue)]() mutable {
        beast::flat_buffer buffer;

        try {
            for (;;) {
                auto maybe_message = lsp::read_message_blocking(
                    [&input_handle](char* data, std::size_t size) {
                        return input_handle->read_some(data, size);
                    },
                    buffer);

                if (!maybe_message.has_value()) {
                    net::post(executor, [input_queue]() {
                        input_queue->close();
                    });
                    return;
                }

                net::post(executor, [input_queue, message = std::move(*maybe_message)]() mutable {
                    input_queue->push(std::move(message));
                });
            }
        } catch (...) {
            auto error = std::current_exception();
            net::post(executor, [input_queue, error]() {
                input_queue->fail(error);
            });
        }
    }).detach();
}

net::awaitable<void> handle_local_session(
    std::shared_ptr<LocalInputQueue> input_queue,
    std::shared_ptr<LocalSession> session,
    std::shared_ptr<RemoteRegistry> remote_registry,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> response_manager,
    std::shared_ptr<bridge_documents::DocumentManager> document_manager,
    std::chrono::milliseconds request_timeout) {
    fprintf(stderr, "Accepted local stdio session\n");
    
    beast::flat_buffer buffer;
    bool shutdown_requested = false;

    net::co_spawn(session->get_executor(), heartbeat_loop(session, remote_registry), net::detached);
    
    try {
        for (;;) {
            auto maybe_message = co_await input_queue->wait_and_pop();
            if (!maybe_message.has_value()) {
                fprintf(stderr, "Local stdio input closed\n");
                break;
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
            
            fprintf(stderr, "JSON-RPC Request: %s\n", request.dump().c_str());

            bridge_local::RequestContext request_context{document_manager};
            auto local_handling = bridge_local::handle_request(session, request, &request_context);
            if (!local_handling.forward_to_remote) {
                if (local_handling.response.has_value()) {
                    co_await write_json_message(session, *local_handling.response);
                }

                if (local_handling.directive == bridge_local::SessionDirective::mark_shutdown_requested) {
                    shutdown_requested = true;
                } else if (local_handling.directive == bridge_local::SessionDirective::close_session) {
                    fprintf(stderr,
                        shutdown_requested
                            ? "Closing local session after exit notification following shutdown\n"
                            : "Closing local session after exit notification without prior shutdown\n"
                    );
                    session->close();
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
        fprintf(stderr, "Closing local stdio session after transport failure\n");
    }

    session->close();
    session_manager->unregister(session);
}
// Remote reader coroutine: continuously receives from Go backend and broadcasts to all local sessions
net::awaitable<void> remote_reader(
    std::shared_ptr<RemoteClient> remote,
    std::shared_ptr<ResponseManager> response_manager,
    std::shared_ptr<RemoteNotificationQueue> notification_queue) {
    
    try {
        for (;;) {
            std::string message = co_await remote->receive();
            fprintf(stderr, "Received from Go backend: %s\n", message.c_str());
            
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

int main(int argc, char* argv[]) {
    try {
        configure_stdio_for_immediate_flush();
        const BridgeSettings settings = parse_bridge_settings(argc, argv);
        if (settings.show_version) {
            printf("dltxt_bridge_v3 %s\n", dltxt_bridge::version);
            return 0;
        }

        net::io_context ioc;
        auto stop_signals = install_stop_signals(ioc);

        auto remote_registry = std::make_shared<RemoteRegistry>();
        auto session_manager = std::make_shared<LocalSessionManager>();
        auto response_manager = std::make_shared<ResponseManager>();
        auto document_manager = std::make_shared<bridge_documents::DocumentManager>();
        auto http_proxy_service = std::make_shared<bridge_http::MojiProxyService>(ioc.get_executor(), dltxt_bridge::version);
        auto local_input = open_stdio_input_handle();
        auto local_input_queue = std::make_shared<LocalInputQueue>(ioc.get_executor());
        auto local_session = session_manager->register_session(std::make_shared<LocalSession>(ioc.get_executor(), open_stdio_output_handle()));
        start_stdio_reader_thread(ioc.get_executor(), local_input, local_input_queue);

        // Launch ONE coordinator coroutine to handle startup order
        net::co_spawn(ioc, [remote_registry, session_manager, response_manager, document_manager, local_input_queue, local_session, settings]() mutable -> net::awaitable<void> {
            auto ex = co_await net::this_coro::executor;
            net::co_spawn(ex, handle_local_session(local_input_queue, local_session, remote_registry, session_manager, response_manager, document_manager, settings.request_timeout), net::detached);

            co_await run_remote_retry_loop(
                remote_registry,
                [settings](net::any_io_executor executor) {
                    return std::make_shared<RemoteClient>(executor, settings.remote_port == "443");
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

        
        net::co_spawn(ioc, bridge_http::http_listener(bridge_http::local_http_port, http_proxy_service), net::detached);

        ioc.run();
    } catch (const std::invalid_argument& e) {
        fprintf(stderr, "Configuration Error: %s\n", e.what());
        fprintf(stderr, "Usage: dltxt_bridge_v3 [--remote-host <host>] [--remote-port <port>] [--request-timeout-ms <ms>] [--version]\n");
        return 1;
    } catch (std::exception& e) {
        fprintf(stderr, "Bridge Error: %s\n", e.what());
        return 1;
    }
    return 0;
}