#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "bridge_app.hpp"
#include "bridge_crossref.hpp"
#include "bridge_documents.hpp"
#include "bridge_local_requests.hpp"
#include "bridge_protocol.hpp"
#include "bridge_thread_pool.hpp"
#include "bridge_version.hpp"
#include "bridge_text_parser.hpp"
#include <cstdio>
#include <exception>
#include <memory>
#include <deque>
#include <thread>

namespace beast = boost::beast;
net::awaitable<void> write_json_message(const std::shared_ptr<LocalSession>& session, const json& message, bool should_log = true);
net::awaitable<void> write_json_error(const std::shared_ptr<LocalSession>& session, const json& id, int code, const std::string& message);
void notify_crossref_index_ready(std::weak_ptr<LocalSession> session);

template <typename Fn>
net::awaitable<json> run_json_task_on_pool(boost::asio::thread_pool& pool, Fn&& fn) {
    co_return co_await net::co_spawn(
        pool,
        [fn = std::forward<Fn>(fn)]() mutable -> net::awaitable<json> {
            co_return fn();
        },
        net::use_awaitable);
}

void configure_stdio_for_immediate_flush() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

net::awaitable<void> heartbeat_loop(std::shared_ptr<LocalSession> session) {
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
                    json{{"message", "heartbeat"}}
                ),
                false
            );
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Heartbeat loop error: %s\n", e.what());
    }
}

net::awaitable<void> write_json_message(const std::shared_ptr<LocalSession>& session, const json& message, bool should_log) {
    if (should_log) {
        fprintf(stderr, "write_json_message begin: %s\n", message.dump().c_str());
    }
    std::string framed = lsp::frame_message(message.dump());
    co_await session->write_framed(std::move(framed));
    if (should_log) {
        fprintf(stderr, "write_json_message end\n");
    }
}

net::awaitable<void> write_json_error(const std::shared_ptr<LocalSession>& session, const json& id, int code, const std::string& message) {
    co_await write_json_message(session, jsonrpc::create_error(id, code, message));
}

void schedule_crossref_index_ready_notification(const std::shared_ptr<LocalSession>& session) {
    net::co_spawn(
        session->get_executor(),
        [session]() -> net::awaitable<void> {
            if (!session->is_open()) {
                co_return;
            }

            try {
                fprintf(stderr, "[debug] notify_crossref_index_ready: sending notification\n");
                co_await write_json_message(
                    session,
                    jsonrpc::create_notification("dltxt/crossref_index_ready", json::object()));
                fprintf(stderr, "[debug] notify_crossref_index_ready: notification sent\n");
            } catch (const std::exception& e) {
                fprintf(stderr, "Failed to notify crossref index ready: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "Failed to notify crossref index ready: unknown exception\n");
            }
        },
        net::detached);
}

void notify_crossref_index_ready(std::weak_ptr<LocalSession> weak_session) {
    fprintf(stderr, "[debug] notify_crossref_index_ready called\n");
    auto session = weak_session.lock();
    if (!session || !session->is_open()) {
        fprintf(stderr, "[debug] notify_crossref_index_ready: session not available or not open\n");
        return;
    }

    if (!session->request_crossref_index_ready_delivery()) {
        fprintf(stderr, "[debug] notify_crossref_index_ready: deferring until client initialized\n");
        return;
    }

    schedule_crossref_index_ready_notification(session);
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
            fprintf(stderr, "LocalInputQueue push ignored because queue is closed\n");
            return;
        }

        fprintf(stderr,
            "LocalInputQueue push: payload_bytes=%zu size_before=%zu\n",
            message.size(),
            messages_.size());
        messages_.push_back(std::move(message));
        fprintf(stderr, "LocalInputQueue push complete: size_after=%zu\n", messages_.size());
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
                const std::size_t size_before = messages_.size();
                std::string message = std::move(messages_.front());
                messages_.pop_front();
                fprintf(stderr,
                    "LocalInputQueue pop: payload_bytes=%zu size_before=%zu size_after=%zu\n",
                    message.size(),
                    size_before,
                    messages_.size());
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
                    fprintf(stderr, "stdio reader thread: read_message_blocking returned EOF\n");
                    net::post(executor, [input_queue]() {
                        input_queue->close();
                    });
                    return;
                }

                fprintf(stderr,
                    "stdio reader thread: got framed payload_bytes=%zu\n",
                    maybe_message->size());
                net::post(executor, [input_queue, message = std::move(*maybe_message)]() mutable {
                    fprintf(stderr,
                        "stdio reader thread: posting payload into LocalInputQueue, payload_bytes=%zu\n",
                        message.size());
                    input_queue->push(std::move(message));
                });
            }
        } catch (...) {
            auto error = std::current_exception();
            fprintf(stderr, "stdio reader thread: exception, failing LocalInputQueue\n");
            net::post(executor, [input_queue, error]() {
                input_queue->fail(error);
            });
        }
    }).detach();
}

net::awaitable<void> handle_local_session(
    std::shared_ptr<LocalInputQueue> input_queue,
    std::shared_ptr<LocalSession> session,
    std::shared_ptr<LocalSessionManager> session_manager,
    std::shared_ptr<ResponseManager> local_response_manager,
    std::shared_ptr<bridge_documents::DocumentManager> document_manager,
    std::shared_ptr<bridge_crossref::CrossrefService> crossref_service) {
    fprintf(stderr, "Accepted local stdio session\n");

    bool shutdown_requested = false;

    net::co_spawn(session->get_executor(), heartbeat_loop(session), net::detached);
    
    try {
        for (;;) {
            fprintf(stderr, "handle_local_session: waiting for next queued message\n");
            auto maybe_message = co_await input_queue->wait_and_pop();
            if (!maybe_message.has_value()) {
                fprintf(stderr, "Local stdio input closed\n");
                break;
            }

            std::string msg_str = std::move(*maybe_message);
            fprintf(stderr, "handle_local_session: dequeued payload_bytes=%zu\n", msg_str.size());
            if (msg_str.empty()) {
                fprintf(stderr, "handle_local_session: skipping empty payload\n");
                continue;
            }

            json request;
            std::string method = "<unparsed>";
            std::optional<json> message_error_id;
            bool message_failed = false;
            std::string message_error;
            
            try {
                request = json::parse(msg_str);
            } catch (const std::exception& e) {
                fprintf(stderr, "JSON parse error: %s\n", e.what());
                continue;
            }

            if (request.contains("id")) {
                message_error_id = request["id"];
            }

            if (bridge_local::try_store_jsonrpc_response(request, local_response_manager)) {
                fprintf(stderr, "handle_local_session: stored local response id=%s\n", request["id"].dump().c_str());
                continue;
            }

            if (request.is_object()
                && request.contains("jsonrpc")
                && request["jsonrpc"] == "2.0"
                && request.contains("id")
                && (request.contains("result") || request.contains("error"))) {
                fprintf(stderr,
                    "handle_local_session: ignoring unmatched local response id=%s\n",
                    request["id"].dump().c_str());
                continue;
            }

            try {
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

                method = request["method"].get<std::string>();
                const json params = request.value("params", json::object());

                if (method == "textDocument/didOpen") {
                    const json text_document = params.value("textDocument", json::object());
                    const std::string uri = text_document.value("uri", std::string{});
                    const std::size_t text_bytes = text_document.contains("text") && text_document["text"].is_string()
                        ? text_document["text"].get<std::string>().size()
                        : std::size_t{0};
                    fprintf(stderr,
                        "JSON-RPC Request: textDocument/didOpen uri=%s text_bytes=%zu\n",
                        uri.c_str(),
                        text_bytes);
                } else {
                    fprintf(stderr, "JSON-RPC Request: %s\n", request.dump().c_str());
                }

                if (method == "dltxt/set_parser_regex") {
                    fprintf(stderr, "[debug] handling set_parser_regex method\n");
                    if (!params.contains("parserRegex")) {
                        if (request.contains("id")) {
                            co_await write_json_error(session, request["id"], -32602, "Invalid params: missing 'parserRegex' field");
                        }
                        continue;
                    }

                    if (params["parserRegex"].is_null()) {
                        if (request.contains("id")) {
                            co_await write_json_message(session, jsonrpc::create_response(request["id"], json(nullptr)));
                        }
                        continue;
                    } else if (params["parserRegex"].is_object()) {
                        bridge_text::ParserState parser_state{
                            bridge_text::parser_config_from_json(params["parserRegex"]),
                            params["parserRegex"].dump(),
                        };
                        bridge_text::set_global_text_parser(bridge_text::make_text_parser(parser_state.config));
                        crossref_service->update_parser_config(
                            parser_state,
                            document_manager->workspace_folders_snapshot(),
                            document_manager->open_documents_snapshot(),
                            bridge_runtime::shared_thread_pool());
                    } else {
                        if (request.contains("id")) {
                            co_await write_json_error(session, request["id"], -32602, "Invalid params: 'parserRegex' must be an object or null");
                        }
                        continue;
                    }

                    if (request.contains("id")) {
                        co_await write_json_message(session, jsonrpc::create_response(request["id"], json(nullptr)));
                    }
                    continue;
                }

                if (method == "dltxt/get_similar_text") {
                    if (!request.contains("id")) {
                        continue;
                    }

                    if (!params.contains("uri") || !params["uri"].is_string()) {
                        co_await write_json_error(session, request["id"], -32602, "Invalid params: missing or invalid 'uri' field");
                        continue;
                    }

                    const std::string uri = params["uri"].get<std::string>();
                    const int threshold = params.value("threshold", 80);
                    const std::size_t limit = params.value("limit", std::size_t{10});
                    fprintf(stderr,
                        "similar_text: request id=%s uri=%s threshold=%d limit=%zu\n",
                        request["id"].dump().c_str(),
                        uri.c_str(),
                        threshold,
                        limit);

                    std::optional<bridge_documents::TextDocument> current_document = document_manager->document_snapshot(uri);
                    if (!current_document.has_value()) {
                        bool loaded = false;
                        try {
                            current_document = bridge_batch::load_document_fast_from_uri(uri);
                            loaded = true;
                            fprintf(stderr, "similar_text: loaded current document from disk uri=%s\n", uri.c_str());
                        } catch (...) {
                            fprintf(stderr, "similar_text: failed to load current document from disk uri=%s\n", uri.c_str());
                        }

                        if (!loaded) {
                            fprintf(stderr, "similar_text: returning empty result because current document is unavailable uri=%s\n", uri.c_str());
                            co_await write_json_message(session, jsonrpc::create_response(request["id"], json{{"matches", json::array()}}));
                            continue;
                        }
                    } else {
                        fprintf(stderr, "similar_text: using snapshotted open document uri=%s version=%d\n", uri.c_str(), current_document->getVersion());
                    }

                    fprintf(stderr, "similar_text: dispatching search task uri=%s\n", uri.c_str());
                    const json result = co_await run_json_task_on_pool(
                        bridge_runtime::shared_thread_pool(),
                        [crossref_service, current_document = std::move(*current_document), threshold, limit]() mutable {
                            return crossref_service->search_json(current_document, threshold, limit);
                        });

                    fprintf(stderr,
                        "similar_text: search task finished uri=%s match_groups=%zu\n",
                        uri.c_str(),
                        result.contains("matches") && result["matches"].is_array() ? result["matches"].size() : std::size_t{0});

                    co_await write_json_message(session, jsonrpc::create_response(request["id"], result));
                    fprintf(stderr, "similar_text: response written id=%s uri=%s\n", request["id"].dump().c_str(), uri.c_str());
                    continue;
                }

                bridge_local::RequestContext request_context{document_manager};
                auto local_handling = bridge_local::handle_request(session, request, &request_context);
                fprintf(stderr,
                    "handle_local_session: method=%s forward_to_remote=%d has_response=%d directive=%d\n",
                    method.c_str(),
                    local_handling.forward_to_remote ? 1 : 0,
                    local_handling.response.has_value() ? 1 : 0,
                    static_cast<int>(local_handling.directive));
                if (!local_handling.forward_to_remote) {
                    if (method == "initialized" && session->mark_client_initialized()) {
                        fprintf(stderr, "[debug] handle_local_session: flushing deferred crossref index ready notification\n");
                        schedule_crossref_index_ready_notification(session);
                    }

                    if (method == "initialize" || method == "workspace/didChangeWorkspaceFolders") {
                        // pass
                    } else if (method == "textDocument/didOpen" || method == "textDocument/didChange" || method == "textDocument/didSave") {
                        const std::string uri = bridge_local::text_document_uri_from_params(params);
                        auto snapshot = document_manager->document_snapshot(uri);
                        if (snapshot.has_value()) {
                            crossref_service->schedule_open_document_update(std::move(*snapshot), bridge_runtime::shared_thread_pool());
                        }
                    } else if (method == "textDocument/didClose") {
                        crossref_service->schedule_closed_document_refresh(
                            bridge_local::text_document_uri_from_params(params),
                            bridge_runtime::shared_thread_pool());
                    }

                    if (local_handling.response.has_value()) {
                        fprintf(stderr,
                            "handle_local_session: writing local response for method=%s id=%s\n",
                            method.c_str(),
                            request.value("id", json(nullptr)).dump().c_str());
                        co_await write_json_message(session, *local_handling.response);
                        fprintf(stderr,
                            "handle_local_session: finished local response for method=%s id=%s\n",
                            method.c_str(),
                            request.value("id", json(nullptr)).dump().c_str());
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

                if (request.contains("id")) {
                    co_await write_json_error(session, request["id"], -32601, "method " + method + " is not recognized");
                }
            } catch (const std::exception& e) {
                message_failed = true;
                message_error = e.what();
            } catch (...) {
                message_failed = true;
                message_error = "unknown exception";
            }

            if (message_failed) {
                fprintf(stderr,
                    "handle_local_session: message processing failed for method=%s id=%s: %s\n",
                    method.c_str(),
                    message_error_id.has_value() ? message_error_id->dump().c_str() : "null",
                    message_error.c_str());
                if (message_error_id.has_value()) {
                    co_await write_json_error(session, *message_error_id, -32603, "Internal error");
                }
                continue;
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Closing local stdio session after transport failure: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "Closing local stdio session after transport failure: unknown exception\n");
    }

    session->close();
    session_manager->unregister(session);
}

int main(int argc, char* argv[]) {
    try {
        configure_stdio_for_immediate_flush();
        const BridgeSettings settings = parse_bridge_settings(argc, argv);
        if (settings.show_version) {
            printf("dltxt_bridge_v3 %s\n", dltxt_bridge::version);
            return 0;
        }

        bridge_runtime::initialize_shared_thread_pool();
        struct SharedThreadPoolShutdownGuard {
            ~SharedThreadPoolShutdownGuard() {
                bridge_runtime::shutdown_shared_thread_pool();
            }
        } shared_thread_pool_shutdown_guard;

        net::io_context ioc;
        auto stop_signals = install_stop_signals(ioc);

        auto session_manager = std::make_shared<LocalSessionManager>();
        auto local_response_manager = std::make_shared<ResponseManager>();
        auto document_manager = std::make_shared<bridge_documents::DocumentManager>();
        auto crossref_service = std::make_shared<bridge_crossref::CrossrefService>();
        auto local_input = open_stdio_input_handle();
        auto local_input_queue = std::make_shared<LocalInputQueue>(ioc.get_executor());
        auto local_session = session_manager->register_session(std::make_shared<LocalSession>(ioc.get_executor(), open_stdio_output_handle()));
        crossref_service->set_index_ready_notifier([weak_session = std::weak_ptr<LocalSession>(local_session)]() {
            notify_crossref_index_ready(weak_session);
        });
        start_stdio_reader_thread(ioc.get_executor(), local_input, local_input_queue);

        net::co_spawn(
            ioc,
            handle_local_session(
                local_input_queue,
                local_session,
                session_manager,
                local_response_manager,
                document_manager,
                crossref_service),
            net::detached);

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