#include <boost/asio.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../local_session.hpp"
#include "../bridge_app.hpp"
#include "../bridge_documents.hpp"
#include "../bridge_http_proxy.hpp"
#include "../batch.hpp"
#include "../bridge_local_requests.hpp"
#include "../bridge_protocol.hpp"
#include "../bridge_runtime.hpp"
#include "../bridge_thread_pool.hpp"
#include "../bridge_similarity_index.hpp"
#include "../bridge_text_parser.hpp"

namespace {
using TestFn = void (*)();

struct NamedTest {
    const char* name;
    TestFn fn;
};

std::vector<NamedTest>& registered_tests() {
    static std::vector<NamedTest> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const char* name, TestFn fn) {
        registered_tests().push_back(NamedTest{name, fn});
    }
};

inline constexpr auto valid_batch_callback = [](const bridge_documents::TextDocument&, std::size_t) {};
inline constexpr auto invalid_batch_callback = [](int) {};

static_assert(bridge_batch::BatchProcessCallback<decltype(valid_batch_callback)>);
static_assert(!bridge_batch::BatchProcessCallback<decltype(invalid_batch_callback)>);

std::string utf8_bytes(const char* text) {
    return std::string(text);
}

#define DEFINE_TEST(name) \
    void name(); \
    const TestRegistrar name##_registrar{#name, &name}; \
    void name()

DEFINE_TEST(test_frame_message) {
    const std::string payload = R"({"jsonrpc":"2.0","method":"initialize"})";
    const std::string framed = lsp::frame_message(payload);

    const std::string expected_prefix = "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    assert(framed.rfind(expected_prefix, 0) == 0);
    assert(framed.substr(expected_prefix.size()) == payload);
}

DEFINE_TEST(test_is_ignorable_header_block) {
    assert(lsp::is_ignorable_header_block("\r\n\r\n"));
    assert(lsp::is_ignorable_header_block(" \t\r\n\r\n"));
    assert(!lsp::is_ignorable_header_block(""));
    assert(!lsp::is_ignorable_header_block("Content-Length: 10\r\n\r\n"));
}

DEFINE_TEST(test_escape_control_chars) {
    const std::string escaped = lsp::escape_control_chars("Content-Length: 10\r\n\tX\r\n\r\n");
    assert(escaped == "Content-Length: 10\\r\\n\\tX\\r\\n\\r\\n");
}

DEFINE_TEST(test_escape_control_chars_hex_escapes_non_printable_bytes) {
    const std::string escaped = lsp::escape_control_chars(std::string{"A\0B", 3});
    assert(escaped == "A\\x00B");
}

DEFINE_TEST(test_header_block_length_finds_complete_header_in_buffer) {
    beast::flat_buffer buffer;
    const std::string framed = "Content-Length: 2\r\n\r\n{}trailing";
    buffer.commit(net::buffer_copy(buffer.prepare(framed.size()), net::buffer(framed)));

    const auto header_length = lsp::header_block_length(buffer.data());
    assert(header_length.has_value());
    assert(*header_length == std::string{"Content-Length: 2\r\n\r\n"}.size());
}

DEFINE_TEST(test_parse_content_length_with_crlf) {
    const std::string headers = "Content-Length: 123\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n";
    const auto parsed = lsp::parse_content_length(headers);

    assert(parsed.has_value());
    assert(*parsed == 123U);
}

DEFINE_TEST(test_parse_content_length_case_insensitive) {
    const std::string headers = "content-length: 42\r\n\r\n";
    const auto parsed = lsp::parse_content_length(headers);

    assert(parsed.has_value());
    assert(*parsed == 42U);
}

DEFINE_TEST(test_parse_content_length_missing) {
    const auto parsed = lsp::parse_content_length("Content-Type: text/plain\r\n\r\n");
    assert(!parsed.has_value());
}

DEFINE_TEST(test_parse_content_length_too_large) {
    const std::string headers = "Content-Length: " + std::to_string(lsp::max_content_length + 1) + "\r\n\r\n";
    const auto parsed = lsp::parse_content_length(headers);

    assert(!parsed.has_value());
}

DEFINE_TEST(test_read_message_blocking_parses_chunked_message) {
    beast::flat_buffer buffer;
    const std::string payload = R"({"jsonrpc":"2.0","method":"initialize"})";
    const std::string framed = lsp::frame_message(payload);
    std::size_t offset = 0;

    const auto message = lsp::read_message_blocking(
        [&](char* data, std::size_t size) -> std::pair<boost::system::error_code, std::size_t> {
            const std::size_t remaining = framed.size() - offset;
            const std::size_t chunk_size = (std::min)(size, (std::min)(remaining, static_cast<std::size_t>(5)));
            std::copy_n(framed.data() + offset, chunk_size, data);
            offset += chunk_size;
            return {{}, chunk_size};
        },
        buffer);

    assert(message.has_value());
    assert(*message == payload);
}

DEFINE_TEST(test_parse_bridge_settings_defaults) {
    const BridgeSettings settings = parse_bridge_settings({});

    assert(settings.remote_host == "127.0.0.1");
    assert(settings.remote_port == "9000");
    assert(settings.request_timeout == std::chrono::seconds(30));
    assert(!settings.show_version);
}

DEFINE_TEST(test_parse_bridge_settings_overrides) {
    const BridgeSettings settings = parse_bridge_settings({
        "--remote-host", "example.com",
        "--remote-port", "9010",
        "--request-timeout-ms", "1500"
    });

    assert(settings.remote_host == "example.com");
    assert(settings.remote_port == "9010");
    assert(settings.request_timeout == std::chrono::milliseconds(1500));
    assert(!settings.show_version);
}

DEFINE_TEST(test_parse_bridge_settings_version_flag) {
    const BridgeSettings settings = parse_bridge_settings({"--version"});

    assert(settings.show_version);
    assert(settings.remote_host == "127.0.0.1");
    assert(settings.remote_port == "9000");
}

DEFINE_TEST(test_local_http_proxy_uses_fixed_port) {
    assert(bridge_http::local_http_port == 9286);
}

DEFINE_TEST(test_extract_application_id_from_script_asset) {
    const auto application_id = bridge_http::extract_application_id(R"(window._ApplicationId = "AbC123")");

    assert(application_id.has_value());
    assert(*application_id == "AbC123");
}

DEFINE_TEST(test_extract_preload_script_urls_resolves_relative_and_absolute_hrefs) {
    const std::string html = R"(
        <html>
            <head>
                <link rel="preload" as="script" href="/assets/app.js">
                <link as="script" rel="preload" href="https://cdn.example.com/vendor.js">
                <link rel="stylesheet" href="/assets/app.css">
            </head>
        </html>)";

    const auto urls = bridge_http::extract_preload_script_urls(html, "https://www.mojidict.com/");

    assert(urls.size() == 2);
    assert(urls[0] == "https://www.mojidict.com/assets/app.js");
    assert(urls[1] == "https://cdn.example.com/vendor.js");
}

DEFINE_TEST(test_build_search_payload_matches_expected_union_api_shape) {
    const json payload = bridge_http::build_search_payload("bridge");

    assert(payload["functions"].is_array());
    assert(payload["functions"].size() == 1);
    assert(payload["functions"][0]["name"] == "search-all");
    assert(payload["functions"][0]["params"]["text"] == "bridge");
    assert(payload["functions"][0]["params"]["types"] == json::array({102, 106}));
}

DEFINE_TEST(test_transform_details_response_shapes_words_for_local_clients) {
    const json response = json{{"result", json{{"result", json::array({
        json{
            {"word", json{{"objectId", "word-1"}, {"spell", "spell-1"}, {"pron", "pron-1"}, {"accent", "1"}, {"excerpt", "to see"}}},
            {"details", json::array({json{{"objectId", "detail-1"}, {"title", "Meaning"}}})},
            {"subdetails", json::array({json{{"objectId", "sub-1"}, {"title", "Primary"}, {"detailsId", "detail-1"}}})},
            {"examples", json::array({json{{"objectId", "example-1"}, {"title", "Example title"}, {"trans", "example"}, {"subdetailsId", "sub-1"}}})}
        }
    })}}}};

    const json transformed = bridge_http::transform_details_response(response);

    assert(transformed["words"].is_array());
    assert(transformed["words"].size() == 1);
    assert(transformed["words"][0]["id"] == "word-1");
    assert(transformed["words"][0]["spell"] == "spell-1");
    assert(transformed["words"][0]["details"][0]["id"] == "detail-1");
    assert(transformed["words"][0]["subDetails"][0]["detailId"] == "detail-1");
    assert(transformed["words"][0]["subDetails"][0]["examples"][0]["id"] == "example-1");
}

DEFINE_TEST(test_dispatch_local_request_returns_healthcheck_text) {
    net::io_context ioc;
    std::optional<bridge_http::LocalHttpResponse> response;
    bool ensure_ready_called = false;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            response = co_await bridge_http::dispatch_local_request(
                bridge_http::http::verb::get,
                "/healthcheck",
                "",
                "0.1.1",
                [&ensure_ready_called]() -> net::awaitable<void> {
                    ensure_ready_called = true;
                    co_return;
                },
                [](std::string, json) -> net::awaitable<json> {
                    assert(false);
                    co_return json::object();
                });
            co_return;
        },
        net::detached);

    ioc.run();

    assert(response.has_value());
    assert(!ensure_ready_called);
    assert(response->status == bridge_http::http::status::ok);
    assert(response->body == "version=0.1.1");
}

DEFINE_TEST(test_dispatch_local_request_proxies_search_payload_and_response) {
    net::io_context ioc;
    std::optional<bridge_http::LocalHttpResponse> response;
    bool ensure_ready_called = false;
    std::string captured_url;
    json captured_payload;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            response = co_await bridge_http::dispatch_local_request(
                bridge_http::http::verb::post,
                "/search",
                R"({"query":"dictionary","expand":false})",
                "0.1.1",
                [&ensure_ready_called]() -> net::awaitable<void> {
                    ensure_ready_called = true;
                    co_return;
                },
                [&captured_url, &captured_payload](std::string url, json payload) -> net::awaitable<json> {
                    captured_url = std::move(url);
                    captured_payload = std::move(payload);
                    json proxy_response;
                    proxy_response["result"]["results"]["search-all"]["items"] = json::array({json{{"id", "entry-1"}}});
                    co_return proxy_response;
                });
            co_return;
        },
        net::detached);

    ioc.run();

    assert(ensure_ready_called);
    assert(captured_url == "https://api.mojidict.com/parse/functions/union-api");
    assert(captured_payload == bridge_http::build_search_payload("dictionary"));
    assert(response.has_value());
    assert(response->status == bridge_http::http::status::ok);

    const json response_json = json::parse(response->body);
    assert(response_json["items"].is_array());
    assert(response_json["items"][0]["id"] == "entry-1");
}

DEFINE_TEST(test_dispatch_local_request_proxies_details_and_transforms_response) {
    net::io_context ioc;
    std::optional<bridge_http::LocalHttpResponse> response;
    json captured_payload;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            response = co_await bridge_http::dispatch_local_request(
                bridge_http::http::verb::post,
                "/details",
                R"({"objectIds":["word-1"]})",
                "0.1.1",
                []() -> net::awaitable<void> {
                    co_return;
                },
                [&captured_payload](std::string, json payload) -> net::awaitable<json> {
                    captured_payload = std::move(payload);
                    json proxy_response = json{{"result", json{{"result", json::array({
                        json{
                            {"word", json{{"objectId", "word-1"}, {"spell", "bridge-spell"}}},
                            {"details", json::array({json{{"objectId", "detail-1"}, {"title", "bridge"}}})},
                            {"subdetails", json::array()},
                            {"examples", json::array()}
                        }
                    })}}}};
                    co_return proxy_response;
                });
            co_return;
        },
        net::detached);

    ioc.run();

    assert(captured_payload == bridge_http::build_details_payload({"word-1"}));
    assert(response.has_value());
    assert(response->status == bridge_http::http::status::ok);

    const json response_json = json::parse(response->body);
    assert(response_json["words"].size() == 1);
    assert(response_json["words"][0]["id"] == "word-1");
    assert(response_json["words"][0]["details"][0]["title"] == "bridge");
}

DEFINE_TEST(test_initialize_http_service_warms_up_remote_dependencies_before_requests) {
    net::io_context ioc;
    bool ensure_ready_called = false;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            co_await bridge_http::initialize_http_service(
                [&ensure_ready_called]() -> net::awaitable<void> {
                    ensure_ready_called = true;
                    co_return;
                });
            co_return;
        },
        net::detached);

    ioc.run();

    assert(ensure_ready_called);
}

DEFINE_TEST(test_parse_bridge_settings_rejects_legacy_listen_port_argument) {
    bool threw = false;

    try {
        static_cast<void>(parse_bridge_settings({"--listen-port", "6010"}));
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
}

DEFINE_TEST(test_bridge_signal_numbers_include_shutdown_signals) {
    const auto signals = bridge_signal_numbers();

    assert(std::find(signals.begin(), signals.end(), SIGINT) != signals.end());
    assert(std::find(signals.begin(), signals.end(), SIGTERM) != signals.end());
}

DEFINE_TEST(test_local_session_manager_register_session_preserves_identity) {
    net::io_context ioc;
    LocalSessionManager manager;
    auto socket = std::make_shared<tcp::socket>(ioc);
    socket->open(tcp::v4());
    auto expected_session = std::make_shared<LocalSession>(socket);

    const auto session = manager.register_session(expected_session);

    assert(manager.session_count() == 1);
    assert(session == expected_session);
}

DEFINE_TEST(test_local_session_manager_stops_when_idle_timeout_expires) {
    net::io_context ioc;
    bool idle_timeout_triggered = false;

    LocalSessionManager manager(
        ioc.get_executor(),
        std::chrono::milliseconds(5),
        [&]() {
            idle_timeout_triggered = true;
            ioc.stop();
        });

    ioc.run();

    assert(idle_timeout_triggered);
}

DEFINE_TEST(test_local_session_manager_rearms_idle_timeout_after_last_disconnect) {
    net::io_context ioc;
    bool idle_timeout_triggered = false;

    LocalSessionManager manager(
        ioc.get_executor(),
        std::chrono::milliseconds(10),
        [&]() {
            idle_timeout_triggered = true;
            ioc.stop();
        });

    auto socket = std::make_shared<tcp::socket>(ioc);
    socket->open(tcp::v4());
    const auto session = manager.register_session(std::make_shared<LocalSession>(socket));

    net::steady_timer probe_timer(ioc);
    probe_timer.expires_after(std::chrono::milliseconds(2));
    probe_timer.async_wait([&](const boost::system::error_code&) {
        ioc.stop();
    });

    ioc.run();

    assert(!idle_timeout_triggered);

    manager.unregister(session);

    ioc.restart();
    ioc.run();

    assert(idle_timeout_triggered);
}

DEFINE_TEST(test_local_session_manager_last_disconnect_triggers_immediate_idle_timeout_after_rearm) {
    net::io_context ioc;
    bool idle_timeout_triggered = false;

    LocalSessionManager manager(
        ioc.get_executor(),
        std::chrono::milliseconds(50),
        [&]() {
            idle_timeout_triggered = true;
        });

    auto socket = std::make_shared<tcp::socket>(ioc);
    socket->open(tcp::v4());
    const auto session = manager.register_session(std::make_shared<LocalSession>(socket));

    assert(!idle_timeout_triggered);

    manager.unregister(session);

    assert(idle_timeout_triggered);
}

DEFINE_TEST(test_remote_notification_forwarder_broadcasts_to_local_clients) {
    net::io_context ioc;
    auto session_manager = std::make_shared<LocalSessionManager>();
    auto notification_queue = std::make_shared<RemoteNotificationQueue>(ioc.get_executor());

    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    tcp::socket client_socket(ioc);
    client_socket.connect({net::ip::address_v4::loopback(), acceptor.local_endpoint().port()});

    auto server_socket = std::make_shared<tcp::socket>(ioc);
    acceptor.accept(*server_socket);
    session_manager->register_session(std::make_shared<LocalSession>(server_socket));

    const std::string payload = R"({"jsonrpc":"2.0","method":"simpletm/progress","params":{"project_id":"demo"}})";
    std::optional<std::optional<std::string>> received_message;
    beast::flat_buffer read_buffer;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            received_message = co_await lsp::read_message(client_socket, read_buffer);
            co_return;
        },
        net::detached);

    notification_queue->push(payload);
    notification_queue->close();

    net::co_spawn(ioc,
        remote_notification_forwarder(notification_queue, session_manager),
        net::detached);

    ioc.run();

    assert(received_message.has_value());
    assert(received_message->has_value());
    assert(**received_message == payload);
}

DEFINE_TEST(test_local_session_write_framed_serializes_overlapping_writes) {
    net::io_context ioc;

    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    tcp::socket client_socket(ioc);
    client_socket.connect({net::ip::address_v4::loopback(), acceptor.local_endpoint().port()});

    auto server_socket = std::make_shared<tcp::socket>(ioc);
    acceptor.accept(*server_socket);
    auto session = std::make_shared<LocalSession>(server_socket);

    const std::string first_payload = R"({"jsonrpc":"2.0","method":"first"})";
    const std::string second_payload = R"({"jsonrpc":"2.0","method":"second"})";
    std::optional<std::optional<std::string>> first_received;
    std::optional<std::optional<std::string>> second_received;
    beast::flat_buffer read_buffer;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            first_received = co_await lsp::read_message(client_socket, read_buffer);
            second_received = co_await lsp::read_message(client_socket, read_buffer);
            co_return;
        },
        net::detached);

    net::co_spawn(ioc,
        [session, first_payload]() -> net::awaitable<void> {
            co_await session->write_framed(lsp::frame_message(first_payload));
        },
        net::detached);

    net::co_spawn(ioc,
        [session, second_payload]() -> net::awaitable<void> {
            co_await session->write_framed(lsp::frame_message(second_payload));
        },
        net::detached);

    ioc.run();

    assert(first_received.has_value());
    assert(first_received->has_value());
    assert(**first_received == first_payload);
    assert(second_received.has_value());
    assert(second_received->has_value());
    assert(**second_received == second_payload);
}

DEFINE_TEST(test_local_session_close_closes_socket) {
    net::io_context ioc;
    auto socket = std::make_shared<tcp::socket>(ioc);
    socket->open(tcp::v4());
    LocalSession session(socket);

    assert(socket->is_open());

    session.close();

    assert(!socket->is_open());
}

DEFINE_TEST(test_jsonrpc_helpers) {
    const json response = jsonrpc::create_response(1, json{{"ok", true}});
    assert(response["jsonrpc"] == "2.0");
    assert(response["id"] == 1);
    assert(response["result"]["ok"] == true);

    const json error = jsonrpc::create_error(2, -32600, "Invalid Request");
    assert(error["error"]["code"] == -32600);
    assert(error["error"]["message"] == "Invalid Request");

    const auto string_id = jsonrpc::request_id_from_json("abc");
    assert(string_id.has_value());
    assert(jsonrpc::request_id_to_json(*string_id) == "abc");

    const auto integer_id = jsonrpc::request_id_from_json(7);
    assert(integer_id.has_value());
    assert(jsonrpc::request_id_to_json(*integer_id) == 7);

    const auto invalid_id = jsonrpc::request_id_from_json(3.14);
    assert(!invalid_id.has_value());

    const auto null_id = jsonrpc::request_id_from_json(nullptr);
    assert(null_id.has_value());
    assert(!jsonrpc::is_trackable_request_id(*null_id));
}

DEFINE_TEST(test_jsonrpc_response_validation) {
    const json valid_response = json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", json{{"ok", true}}}};
    assert(jsonrpc::is_valid_jsonrpc_response(valid_response));

    const json invalid_response_missing_result = json{{"jsonrpc", "2.0"}, {"id", 1}};
    assert(!jsonrpc::is_valid_jsonrpc_response(invalid_response_missing_result));

    const json invalid_response_with_method = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "notify"}};
    assert(!jsonrpc::is_valid_jsonrpc_response(invalid_response_with_method));

    const json valid_notification = json{{"jsonrpc", "2.0"}, {"method", "notify"}, {"params", json::object()}};
    assert(jsonrpc::is_valid_jsonrpc_notification(valid_notification));

    const json invalid_notification_with_id = json{{"jsonrpc", "2.0"}, {"method", "notify"}, {"id", 1}};
    assert(!jsonrpc::is_valid_jsonrpc_notification(invalid_notification_with_id));
}

DEFINE_TEST(test_local_initialize_request_returns_empty_capabilities) {
    const json request = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(handling.response.has_value());
    assert((*handling.response)["jsonrpc"] == "2.0");
    assert((*handling.response)["id"] == 1);
    assert((*handling.response)["result"].is_object());
    assert((*handling.response)["result"]["capabilities"]["textDocumentSync"]["openClose"] == true);
    assert((*handling.response)["result"]["capabilities"]["textDocumentSync"]["change"] == 2);
    assert((*handling.response)["result"]["capabilities"]["textDocumentSync"]["save"]["includeText"] == false);
}

DEFINE_TEST(test_local_handler_returns_shutdown_response) {
    const json request = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "shutdown"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(handling.response.has_value());
    assert((*handling.response)["jsonrpc"] == "2.0");
    assert((*handling.response)["id"] == 1);
    assert((*handling.response)["result"].is_null());
    assert(handling.directive == bridge_local::SessionDirective::mark_shutdown_requested);
}

DEFINE_TEST(test_local_handler_rejects_unsupported_local_requests) {
    const json request = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "textDocument/hover"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(handling.response.has_value());
    assert((*handling.response)["error"]["code"] == -32601);
    assert((*handling.response)["error"]["message"] == "method textDocument/hover is not recognized");
    assert(handling.directive == bridge_local::SessionDirective::none);
}

DEFINE_TEST(test_local_handler_forwards_simpletm_requests) {
    const json request = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "simpletm/complete"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(handling.forward_to_remote);
    assert(!handling.response.has_value());
}

DEFINE_TEST(test_local_handler_swallows_non_simpletm_notifications) {
    const json request = json{{"jsonrpc", "2.0"}, {"method", "initialized"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(!handling.response.has_value());
}

DEFINE_TEST(test_local_handler_swallows_set_trace_notification) {
    const json request = json{{"jsonrpc", "2.0"}, {"method", "$/setTrace"}, {"params", json{{"value", "off"}}}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(!handling.response.has_value());
}

DEFINE_TEST(test_local_handler_closes_session_on_exit_notification) {
    const json request = json{{"jsonrpc", "2.0"}, {"method", "exit"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(!handling.response.has_value());
    assert(handling.directive == bridge_local::SessionDirective::close_session);
}

DEFINE_TEST(test_local_handler_returns_dltxt_version_response) {
    const json request = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "dltxt/version"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(handling.response.has_value());
    assert((*handling.response)["jsonrpc"] == "2.0");
    assert((*handling.response)["id"] == 1);
    assert((*handling.response)["result"].is_object());
    assert((*handling.response)["result"]["version"] == dltxt_bridge::version);
    assert(handling.directive == bridge_local::SessionDirective::none);
}

DEFINE_TEST(test_local_handler_swallows_dltxt_version_notification_without_id) {
    const json request = json{{"jsonrpc", "2.0"}, {"method", "dltxt/version"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(!handling.response.has_value());
    assert(handling.directive == bridge_local::SessionDirective::none);
}

DEFINE_TEST(test_text_document_loader_reads_utf8_bom_file) {
    const std::filesystem::path file_path = std::filesystem::temp_directory_path() / "dltxt_bridge_utf8_bom_test.txt";
    const std::string hello = utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF");
    const std::string world = utf8_bytes("\xE4\xB8\x96\xE7\x95\x8C");
    {
        std::ofstream output(file_path, std::ios::binary);
        const std::string bom_and_text = std::string{"\xEF\xBB\xBF", 3} + hello + "\r\n" + world;
        output.write(bom_and_text.data(), static_cast<std::streamsize>(bom_and_text.size()));
    }

    const auto document = bridge_documents::TextDocumentLoader::load_from_file(file_path);

    assert(document.getEncoding() == "UTF-8");
    assert(document.getFilePath() == file_path);
    assert(document.getFileName() == file_path.filename().string());
    assert(bridge_documents::utf16_to_utf8(document.getLine(0)) == hello);
    assert(bridge_documents::utf16_to_utf8(document.getLine(1)) == world);

    std::filesystem::remove(file_path);
}

DEFINE_TEST(test_batch_process_loads_documents_from_uris) {
    const auto file_path_a = std::filesystem::temp_directory_path() / "dltxt_bridge_batch_a.txt";
    const auto file_path_b = std::filesystem::temp_directory_path() / "dltxt_bridge_batch_b.txt";
    {
        std::ofstream output(file_path_a, std::ios::binary);
        output << "alpha";
    }
    {
        std::ofstream output(file_path_b, std::ios::binary);
        output << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF");
    }

    const std::vector<std::string> uris{
        bridge_documents::file_uri_from_path(file_path_a.string()),
        bridge_documents::file_uri_from_path(file_path_b.string()),
    };
    std::vector<std::string> contents(uris.size());
    std::vector<std::string> seen_uris(uris.size());
    boost::asio::thread_pool pool(2);

    bridge_batch::batch_process(
        pool,
        uris,
        [&](const bridge_documents::TextDocument& document, std::size_t index) {
            seen_uris[index] = document.getUri();
            contents[index] = bridge_documents::utf16_to_utf8(document.getContent());
        },
        bridge_batch::BatchProcessOptions{2});

    assert(seen_uris[0] == uris[0]);
    assert(seen_uris[1] == uris[1]);
    assert(contents[0] == "alpha");
    assert(contents[1] == utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"));

    std::filesystem::remove(file_path_a);
    std::filesystem::remove(file_path_b);
}

DEFINE_TEST(test_batch_process_reports_errors_and_continues) {
    const auto valid_file_path = std::filesystem::temp_directory_path() / "dltxt_bridge_batch_valid.txt";
    const auto missing_file_path = std::filesystem::temp_directory_path() / "dltxt_bridge_batch_missing.txt";
    {
        std::ofstream output(valid_file_path, std::ios::binary);
        output << "ok";
    }
    std::filesystem::remove(missing_file_path);

    const std::vector<std::string> uris{
        bridge_documents::file_uri_from_path(valid_file_path.string()),
        bridge_documents::file_uri_from_path(missing_file_path.string()),
        bridge_documents::file_uri_from_path(valid_file_path.string()),
    };

    std::atomic<std::size_t> success_count = 0;
    std::mutex error_mutex;
    std::vector<std::string> failed_uris;
    std::vector<std::size_t> failed_indices;
    boost::asio::thread_pool pool(2);

    bridge_batch::batch_process(
        pool,
        uris,
        [&](const bridge_documents::TextDocument&, std::size_t) {
            ++success_count;
        },
        bridge_batch::BatchProcessOptions{2},
        [&](const std::string& uri, std::size_t index, std::exception_ptr error) {
            assert(error != nullptr);
            std::lock_guard<std::mutex> lock(error_mutex);
            failed_uris.push_back(uri);
            failed_indices.push_back(index);
        });

    assert(success_count == 2);
    assert(failed_uris.size() == 1);
    assert(failed_uris[0] == uris[1]);
    assert(failed_indices[0] == 1);

    std::filesystem::remove(valid_file_path);
}

DEFINE_TEST(test_batch_process_reuses_external_thread_pool) {
    const auto file_path_a = std::filesystem::temp_directory_path() / "dltxt_bridge_batch_reuse_a.txt";
    const auto file_path_b = std::filesystem::temp_directory_path() / "dltxt_bridge_batch_reuse_b.txt";
    {
        std::ofstream output(file_path_a, std::ios::binary);
        output << "first";
    }
    {
        std::ofstream output(file_path_b, std::ios::binary);
        output << "second";
    }

    boost::asio::thread_pool pool(2);
    std::vector<std::string> first_pass;
    std::vector<std::string> second_pass;

    bridge_batch::batch_process(
        pool,
        std::vector<std::string>{bridge_documents::file_uri_from_path(file_path_a.string())},
        [&](const bridge_documents::TextDocument& document, std::size_t) {
            first_pass.push_back(bridge_documents::utf16_to_utf8(document.getContent()));
        });

    bridge_batch::batch_process(
        pool,
        std::vector<std::string>{bridge_documents::file_uri_from_path(file_path_b.string())},
        [&](const bridge_documents::TextDocument& document, std::size_t) {
            second_pass.push_back(bridge_documents::utf16_to_utf8(document.getContent()));
        });

    assert(first_pass.size() == 1);
    assert(second_pass.size() == 1);
    assert(first_pass[0] == "first");
    assert(second_pass[0] == "second");

    std::filesystem::remove(file_path_a);
    std::filesystem::remove(file_path_b);
}

DEFINE_TEST(test_shared_thread_pool_can_initialize_and_shutdown) {
    bridge_runtime::shutdown_shared_thread_pool();
    assert(!bridge_runtime::shared_thread_pool_initialized());

    bridge_runtime::initialize_shared_thread_pool(2);
    assert(bridge_runtime::shared_thread_pool_initialized());

    std::atomic<int> counter = 0;
    std::latch done(1);
    boost::asio::post(bridge_runtime::shared_thread_pool(), [&]() {
        ++counter;
        done.count_down();
    });
    done.wait();

    assert(counter == 1);

    bridge_runtime::shutdown_shared_thread_pool();
    assert(!bridge_runtime::shared_thread_pool_initialized());
}

DEFINE_TEST(test_utf_conversions_accept_empty_strings) {
    assert(bridge_documents::utf8_to_utf16("").empty());
    assert(bridge_documents::utf16_to_utf8(std::u16string{}).empty());
}

DEFINE_TEST(test_local_handler_tracks_document_lifecycle_notifications) {
    auto manager = std::make_shared<bridge_documents::DocumentManager>();
    bridge_local::RequestContext context{manager};
    const std::string first_line = utf8_bytes("\xE7\xAC\xAC\xE4\xB8\x80\xE8\xA1\x8C");
    const std::string second_line = utf8_bytes("\xE7\xAC\xAC\xE4\xBA\x8C\xE8\xA1\x8C");
    const std::string updated_prefix = utf8_bytes("\xE6\x9B\xB4\xE6\x96\xB0\xE6\xB8\x88\xE3\x81\xBF");
    const std::string updated_line = utf8_bytes("\xE6\x9B\xB4\xE6\x96\xB0\xE6\xB8\x88\xE3\x81\xBF\xE8\xA1\x8C");
    const std::string saved_text = utf8_bytes("\xE4\xBF\x9D\xE5\xAD\x98\xE5\xBE\x8C");

    const json open_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", json{{"textDocument", json{{"uri", "file:///c:/tmp/doc.txt"}, {"version", 1}, {"text", first_line + "\n" + second_line}}}}}
    };
    const auto open_handling = bridge_local::handle_request(nullptr, open_request, &context);

    assert(!open_handling.forward_to_remote);
    assert(manager->has_document("file:///c:/tmp/doc.txt"));
    const auto* document_after_open = manager->find_document("file:///c:/tmp/doc.txt");
    assert(document_after_open != nullptr);
    assert(bridge_documents::utf16_to_utf8(document_after_open->getLine(0)) == first_line);
    assert(bridge_documents::utf16_to_utf8(document_after_open->getLine(1)) == second_line);

    const json change_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didChange"},
        {"params", json{
            {"textDocument", json{{"uri", "file:///c:/tmp/doc.txt"}, {"version", 2}}},
            {"contentChanges", json::array({json{{"range", json{{"start", json{{"line", 1}, {"character", 0}}}, {"end", json{{"line", 1}, {"character", 3}}}}}, {"text", updated_prefix}}})}
        }}
    };
    const auto change_handling = bridge_local::handle_request(nullptr, change_request, &context);

    assert(!change_handling.forward_to_remote);
    const auto* document_after_change = manager->find_document("file:///c:/tmp/doc.txt");
    assert(document_after_change != nullptr);
    assert(document_after_change->getVersion() == 2);
    assert(bridge_documents::utf16_to_utf8(document_after_change->getLine(1)) == updated_line);

    const json save_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didSave"},
        {"params", json{{"textDocument", json{{"uri", "file:///c:/tmp/doc.txt"}}}, {"text", saved_text}}}
    };
    const auto save_handling = bridge_local::handle_request(nullptr, save_request, &context);

    assert(!save_handling.forward_to_remote);
    const auto* document_after_save = manager->find_document("file:///c:/tmp/doc.txt");
    assert(document_after_save != nullptr);
    assert(bridge_documents::utf16_to_utf8(document_after_save->getLine(0)) == saved_text);

    const json close_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didClose"},
        {"params", json{{"textDocument", json{{"uri", "file:///c:/tmp/doc.txt"}}}}}
    };
    const auto close_handling = bridge_local::handle_request(nullptr, close_request, &context);

    assert(!close_handling.forward_to_remote);
    assert(!manager->has_document("file:///c:/tmp/doc.txt"));
}

DEFINE_TEST(test_local_handler_accepts_empty_text_change_notification) {
    auto manager = std::make_shared<bridge_documents::DocumentManager>();
    bridge_local::RequestContext context{manager};

    const json open_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", json{{"textDocument", json{{"uri", "file:///c:/tmp/doc.txt"}, {"version", 1}, {"text", "alpha\nbeta"}}}}}
    };
    const auto open_handling = bridge_local::handle_request(nullptr, open_request, &context);

    assert(!open_handling.forward_to_remote);

    const json change_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didChange"},
        {"params", json{
            {"textDocument", json{{"uri", "file:///c:/tmp/doc.txt"}, {"version", 2}}},
            {"contentChanges", json::array({json{{"range", json{{"start", json{{"line", 1}, {"character", 1}}}, {"end", json{{"line", 1}, {"character", 4}}}}}, {"text", ""}}})}
        }}
    };
    const auto change_handling = bridge_local::handle_request(nullptr, change_request, &context);

    assert(!change_handling.forward_to_remote);
    const auto* document_after_change = manager->find_document("file:///c:/tmp/doc.txt");
    assert(document_after_change != nullptr);
    assert(document_after_change->getVersion() == 2);
    assert(bridge_documents::utf16_to_utf8(document_after_change->getLine(0)) == "alpha");
    assert(bridge_documents::utf16_to_utf8(document_after_change->getLine(1)) == "b");
}

DEFINE_TEST(test_update_saved_document_text_debounces_full_syncs_within_one_minute) {
    bridge_documents::DocumentManager manager;
    manager.open_from_lsp("file:///c:/tmp/doc.txt", "start", 1);

    const auto base_time = std::chrono::steady_clock::time_point{};

    const bool first_applied = manager.update_saved_document_text(
        "file:///c:/tmp/doc.txt",
        "first",
        base_time);
    assert(first_applied);

    const auto* after_first_save = manager.find_document("file:///c:/tmp/doc.txt");
    assert(after_first_save != nullptr);
    assert(bridge_documents::utf16_to_utf8(after_first_save->getContent()) == "first");

    const bool second_applied = manager.update_saved_document_text(
        "file:///c:/tmp/doc.txt",
        "second",
        base_time + std::chrono::seconds(30));
    assert(!second_applied);

    const auto* after_second_save = manager.find_document("file:///c:/tmp/doc.txt");
    assert(after_second_save != nullptr);
    assert(bridge_documents::utf16_to_utf8(after_second_save->getContent()) == "first");

    const bool third_applied = manager.update_saved_document_text(
        "file:///c:/tmp/doc.txt",
        "third",
        base_time + std::chrono::seconds(61));
    assert(third_applied);

    const auto* after_third_save = manager.find_document("file:///c:/tmp/doc.txt");
    assert(after_third_save != nullptr);
    assert(bridge_documents::utf16_to_utf8(after_third_save->getContent()) == "third");
}

DEFINE_TEST(test_local_handler_reads_saved_document_from_disk_when_text_is_omitted) {
    const auto file_path = std::filesystem::temp_directory_path() / "dltxt_bridge_didsave_disk_reload.txt";
    {
        std::ofstream output(file_path, std::ios::binary);
        output << "saved from disk";
    }

    auto manager = std::make_shared<bridge_documents::DocumentManager>();
    bridge_local::RequestContext context{manager};
    const std::string uri = bridge_documents::file_uri_from_path(file_path.string());

    manager->open_from_lsp(uri, "stale in memory", 3);

    const json save_request = json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didSave"},
        {"params", json{{"textDocument", json{{"uri", uri}}}}}
    };

    const auto save_handling = bridge_local::handle_request(nullptr, save_request, &context);

    assert(!save_handling.forward_to_remote);
    const auto* document_after_save = manager->find_document(uri);
    assert(document_after_save != nullptr);
    assert(bridge_documents::utf16_to_utf8(document_after_save->getContent()) == "saved from disk");

    std::filesystem::remove(file_path);
}

DEFINE_TEST(test_workspace_folder_change_stays_local) {
    auto manager = std::make_shared<bridge_documents::DocumentManager>();
    bridge_local::RequestContext context{manager};

    const json request = json{
        {"jsonrpc", "2.0"},
        {"method", "workspace/didChangeWorkspaceFolders"},
        {"params", json{{"event", json{
            {"added", json::array({json{{"uri", "file:///c:/repo-added"}, {"name", "repo-added"}}})},
            {"removed", json::array()}
        }}}}
    };

    const auto handling = bridge_local::handle_request(nullptr, request, &context);

    assert(!handling.forward_to_remote);
    assert(manager->workspace_folders().size() == 1);
    assert(manager->workspace_folders()[0] == "file:///c:/repo-added");
}

DEFINE_TEST(test_local_initialize_updates_document_manager_workspace_folders) {
    auto manager = std::make_shared<bridge_documents::DocumentManager>();
    bridge_local::RequestContext context{manager};

    const json initialize_request = json{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", json{{"workspaceFolders", json::array({json{{"uri", "file:///c:/repo"}, {"name", "repo"}}})}}}
    };
    const auto initialize_handling = bridge_local::handle_request(nullptr, initialize_request, &context);

    assert(!initialize_handling.forward_to_remote);
    assert(manager->workspace_folders().size() == 1);
    assert(manager->workspace_folders()[0] == "file:///c:/repo");
}

DEFINE_TEST(test_local_handler_returns_opened_documents_contents) {
    auto manager = std::make_shared<bridge_documents::DocumentManager>();
    bridge_local::RequestContext context{manager};

    manager->open_from_lsp("file:///c:/repo/b.txt", "second document", 1);
    manager->open_from_lsp("file:///c:/repo/a.txt", "first\ndocument", 2);

    const json request = json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "dltxt/opened_documents"}};
    const auto handling = bridge_local::handle_request(nullptr, request, &context);

    assert(!handling.forward_to_remote);
    assert(handling.response.has_value());
    assert((*handling.response)["jsonrpc"] == "2.0");
    assert((*handling.response)["id"] == 7);
    assert((*handling.response)["result"].is_object());
    assert((*handling.response)["result"].size() == 2);
    assert((*handling.response)["result"]["file:///c:/repo/a.txt"] == "first\ndocument");
    assert((*handling.response)["result"]["file:///c:/repo/b.txt"] == "second document");
}

DEFINE_TEST(test_similarity_index_returns_best_match_with_metadata) {
    bridge_similarity::SimilarityIndex index;
    index.upsert(1, "c:/repo/a.txt", 2, u"alphaalpha");
    index.upsert(2, "c:/repo/b.txt", 7, u"alphabeta");
    index.upsert(3, "c:/repo/c.txt", 9, u"zzzzzzzz");

    const auto results = index.search(u"alphaalpha", 0.0, 2);

    assert(results.size() == 2);
    assert(results[0].line_id == 1);
    assert(results[0].file_path == "c:/repo/a.txt");
    assert(results[0].line_number == 2);
    assert(results[0].score > results[1].score);
    assert(results[1].line_id == 2);
}

DEFINE_TEST(test_similarity_index_upsert_replaces_existing_line_content) {
    bridge_similarity::SimilarityIndex index;
    index.upsert(1, "c:/repo/a.txt", 1, u"abcdef");
    index.upsert(2, "c:/repo/b.txt", 2, u"xyzxyz");

    auto before_update = index.search(u"abcdef", 0.0, 2);
    assert(!before_update.empty());
    assert(before_update[0].line_id == 1);

    index.upsert(1, "c:/repo/a.txt", 1, u"mnopqr");

    const auto old_query_results = index.search(u"abcdef", 0.0, 2);
    assert(old_query_results.empty());

    const auto new_query_results = index.search(u"mnopqr", 0.0, 2);
    assert(new_query_results.size() == 1);
    assert(new_query_results[0].line_id == 1);
}

DEFINE_TEST(test_similarity_index_erase_removes_line_from_search_results) {
    bridge_similarity::SimilarityIndex index;
    index.upsert(10, "c:/repo/a.txt", 0, u"memoryline");
    index.upsert(11, "c:/repo/b.txt", 1, u"engineline");

    assert(index.erase(10));
    assert(!index.erase(10));

    const auto results = index.search(u"memoryline", 0.0, 5);
    assert(results.empty());
    assert(index.document_count() == 1);
}

DEFINE_TEST(test_similarity_index_counts_distinct_files_by_internal_id) {
    bridge_similarity::SimilarityIndex index;
    index.upsert(1, "c:/repo/shared.txt", 0, u"abcdef");
    index.upsert(2, "c:/repo/shared.txt", 1, u"bcdefg");
    index.upsert(3, "c:/repo/other.txt", 2, u"cdefgh");

    assert(index.document_count() == 3);
    assert(index.file_count() == 2);

    assert(index.erase(1));
    assert(index.file_count() == 2);
    assert(index.erase(2));
    assert(index.file_count() == 1);
}

DEFINE_TEST(test_similarity_index_honors_threshold_and_max_results) {
    bridge_similarity::SimilarityIndex index;
    index.upsert(1, "c:/repo/a.txt", 0, u"abcdefgh");
    index.upsert(2, "c:/repo/b.txt", 1, u"abcdefzz");
    index.upsert(3, "c:/repo/c.txt", 2, u"abcxxxxx");

    const auto limited_results = index.search(u"abcdefgh", 0.3, 2);

    assert(limited_results.size() == 2);
    assert(limited_results[0].line_id == 1);
    assert(limited_results[1].line_id == 2);
    assert(limited_results[0].score >= limited_results[1].score);

    const auto filtered_results = index.search(u"abcdefgh", 0.9, 5);
    assert(filtered_results.size() == 1);
    assert(filtered_results[0].line_id == 1);
}

DEFINE_TEST(test_similarity_index_handles_japanese_utf16_escape_literals) {
    bridge_similarity::SimilarityIndex index;

    const std::u16string exact = u"\u4ECA\u65E5\u306F\u3044\u3044\u5929\u6C17\u3067\u3059";
    const std::u16string similar = u"\u4ECA\u65E5\u306F\u826F\u3044\u5929\u6C17\u3067\u3059\u306D";
    const std::u16string different = u"\u6628\u65E5\u306F\u96E8\u3067\u3057\u305F";

    index.upsert(21, "c:/repo/weather-a.txt", 4, exact);
    index.upsert(22, "c:/repo/weather-b.txt", 8, similar);
    index.upsert(23, "c:/repo/weather-c.txt", 9, different);

    const auto results = index.search(exact, 0.0, 3);

    assert(results.size() == 3);
    assert(results[0].line_id == 21);
    assert(results[0].file_path == "c:/repo/weather-a.txt");
    assert(results[0].line_number == 4);
    assert(results[1].line_id == 22);
    assert(results[0].score > results[1].score);
    assert(results[1].score > results[2].score);
    assert(results[2].line_id == 23);
}

DEFINE_TEST(test_similarity_index_replaces_japanese_utf16_escape_content) {
    bridge_similarity::SimilarityIndex index;

    const std::u16string old_line = u"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C";
    const std::u16string new_line = u"\u3053\u3093\u3070\u3093\u306F\u4E16\u754C";

    index.upsert(31, "c:/repo/greeting.txt", 1, old_line);

    const auto old_results = index.search(old_line, 0.0, 2);
    assert(old_results.size() == 1);
    assert(old_results[0].line_id == 31);

    index.upsert(31, "c:/repo/greeting.txt", 1, new_line);

    const auto stale_results = index.search(old_line, 0.8, 2);
    assert(stale_results.empty());

    const auto refreshed_results = index.search(new_line, 0.0, 2);
    assert(refreshed_results.size() == 1);
    assert(refreshed_results[0].line_id == 31);
}

DEFINE_TEST(test_standard_text_parser_extracts_japanese_pairs) {
    const std::string white_circle = utf8_bytes("\xE2\x97\x8B");
    const std::string black_circle = utf8_bytes("\xE2\x97\x8F");
    const bridge_text::RegexConfig config{
        white_circle + "\\d+[TN]" + white_circle,
        black_circle + "\\d+[TN]" + black_circle,
        "",
        "",
        "",
        "",
        "",
    };
    const bridge_text::StandardTextParser parser(config);

    const std::u16string text =
        u"\u25CB00027145T\u25CB\u3053\u3093\u306B\u3061\u306F\u3002\n"
        u"\u25CF00027145T\u25CF\u4F60\u597D\u3002\n\n"
        u"\u25CB00027148N\u25CB\u83EF\u6DE1\n"
        u"\u25CF00027148N\u25CF\u83EF\u6DE1\n";

    const auto pairs = parser.parse_paired_lines(text);

    assert(pairs.size() == 2);
    assert(pairs[0].original_line_index == 0);
    assert(pairs[0].translated_line_index == 1);
    assert(pairs[0].original.text == u"\u3053\u3093\u306B\u3061\u306F\u3002");
    assert(pairs[0].translated.text == u"\u4F60\u597D\u3002");
    assert(pairs[1].original_line_index == 3);
    assert(pairs[1].translated_line_index == 4);
    assert(pairs[1].original.text == u"\u83EF\u6DE1");
}

DEFINE_TEST(test_standard_text_parser_adjusts_quote_boundaries_for_japanese_lines) {
    const std::string white_circle = utf8_bytes("\xE2\x97\x8B");
    const std::string black_circle = utf8_bytes("\xE2\x97\x8F");
    const bridge_text::RegexConfig config{
        white_circle + "\\d+[TN]" + white_circle,
        black_circle + "\\d+[TN]" + black_circle,
        "",
        "",
        "",
        "",
        "",
    };
    const bridge_text::StandardTextParser parser(config);

    const std::u16string text =
        u"\u25CB00027147T\u25CB\u300E\u305D\u3046\u304B\u300F\n"
        u"\u25CF00027147T\u25CF\u201C\u662F\u5417\u201D\n";

    const auto pairs = parser.parse_paired_lines(text);

    assert(pairs.size() == 1);
    assert(pairs[0].original.white == u"\u300E");
    assert(pairs[0].original.text == u"\u305D\u3046\u304B");
    assert(pairs[0].original.suffix == u"\u300F");
    assert(pairs[0].translated.white == u"\u201C");
    assert(pairs[0].translated.text == u"\u662F\u5417");
    assert(pairs[0].translated.suffix == u"\u201D");
}

DEFINE_TEST(test_standard_text_parser_throws_on_dangling_original_line) {
    const std::string white_circle = utf8_bytes("\xE2\x97\x8B");
    const std::string black_circle = utf8_bytes("\xE2\x97\x8F");
    const bridge_text::RegexConfig config{
        white_circle + "\\d+[TN]" + white_circle,
        black_circle + "\\d+[TN]" + black_circle,
        "",
        "",
        "",
        "",
        "",
    };
    const bridge_text::StandardTextParser parser(config);

    bool threw = false;
    try {
        (void)parser.parse_paired_lines(u"\u25CB00027145T\u25CB\u3053\u3093\u306B\u3061\u306F\u3002\n");
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
}

DEFINE_TEST(test_text_document_edit_splits_single_line_when_patch_contains_newline) {
    bridge_documents::TextDocument document(
        "file:///c:/repo/doc.txt",
        "client-decoded",
        "c:/repo/doc.txt",
        1,
        bridge_documents::utf8_to_utf16("hello world"));

    document.edit(
        bridge_documents::TextChange{
            bridge_documents::Range{
                bridge_documents::Position{0, 5},
                bridge_documents::Position{0, 6}
            },
            bridge_documents::utf8_to_utf16("\n")
        },
        2);

    assert(document.getVersion() == 2);
    assert(document.getLines().size() == 2);
    assert(bridge_documents::utf16_to_utf8(document.getLine(0)) == "hello");
    assert(bridge_documents::utf16_to_utf8(document.getLine(1)) == "world");
}

DEFINE_TEST(test_text_document_edit_single_line_patch_only_changes_target_line) {
    bridge_documents::TextDocument document(
        "file:///c:/repo/doc.txt",
        "client-decoded",
        "c:/repo/doc.txt",
        1,
        bridge_documents::utf8_to_utf16("line0\nline1\nline2"));

    document.edit(
        bridge_documents::TextChange{
            bridge_documents::Range{
                bridge_documents::Position{1, 1},
                bridge_documents::Position{1, 4}
            },
            bridge_documents::utf8_to_utf16("XYZ")
        },
        2);

    assert(document.getVersion() == 2);
    assert(document.getLines().size() == 3);
    assert(bridge_documents::utf16_to_utf8(document.getLine(0)) == "line0");
    assert(bridge_documents::utf16_to_utf8(document.getLine(1)) == "lXYZe1");
    assert(bridge_documents::utf16_to_utf8(document.getLine(2)) == "line2");
}

DEFINE_TEST(test_text_document_edit_collapses_multi_line_range_into_single_line) {
    bridge_documents::TextDocument document(
        "file:///c:/repo/doc.txt",
        "client-decoded",
        "c:/repo/doc.txt",
        1,
        bridge_documents::utf8_to_utf16("abc\ndef\nghi"));

    document.edit(
        bridge_documents::TextChange{
            bridge_documents::Range{
                bridge_documents::Position{0, 1},
                bridge_documents::Position{2, 2}
            },
            bridge_documents::utf8_to_utf16("Z")
        },
        3);

    assert(document.getVersion() == 3);
    assert(document.getLines().size() == 1);
    assert(bridge_documents::utf16_to_utf8(document.getLine(0)) == "aZi");
}

DEFINE_TEST(test_text_document_edit_expands_multi_line_range_with_multi_line_patch) {
    bridge_documents::TextDocument document(
        "file:///c:/repo/doc.txt",
        "client-decoded",
        "c:/repo/doc.txt",
        1,
        bridge_documents::utf8_to_utf16("abc\ndef\nghi\njkl"));

    document.edit(
        bridge_documents::TextChange{
            bridge_documents::Range{
                bridge_documents::Position{0, 1},
                bridge_documents::Position{2, 2}
            },
            bridge_documents::utf8_to_utf16("X\nY\nZ")
        },
        4);

    assert(document.getVersion() == 4);
    assert(document.getLines().size() == 5);
    assert(bridge_documents::utf16_to_utf8(document.getLine(0)) == "aX");
    assert(bridge_documents::utf16_to_utf8(document.getLine(1)) == "Y");
    assert(bridge_documents::utf16_to_utf8(document.getLine(2)) == "Zi");
    assert(bridge_documents::utf16_to_utf8(document.getLine(3)) == "jkl");
}

DEFINE_TEST(test_text_document_edit_rejects_out_of_bounds_range) {
    bridge_documents::TextDocument document(
        "file:///c:/repo/doc.txt",
        "client-decoded",
        "c:/repo/doc.txt",
        1,
        bridge_documents::utf8_to_utf16("short\ntext"));

    bool threw = false;
    try {
        document.edit(
            bridge_documents::TextChange{
                bridge_documents::Range{
                    bridge_documents::Position{0, 99},
                    bridge_documents::Position{0, 99}
                },
                bridge_documents::utf8_to_utf16("")
            },
            2);
    } catch (const std::out_of_range&) {
        threw = true;
    }

    assert(threw);
    assert(document.getVersion() == 1);
    assert(document.getLines().size() == 2);
    assert(bridge_documents::utf16_to_utf8(document.getLine(0)) == "short");
    assert(bridge_documents::utf16_to_utf8(document.getLine(1)) == "text");
}

DEFINE_TEST(test_apply_lsp_changes_ignores_invalid_range_without_throwing) {
    bridge_documents::DocumentManager manager;
    manager.open_from_lsp("file:///c:/repo/doc.txt", "short\ntext", 1);

    manager.apply_lsp_changes(
        "file:///c:/repo/doc.txt",
        2,
        std::vector<bridge_documents::TextChange>{
            bridge_documents::TextChange{
                bridge_documents::Range{
                    bridge_documents::Position{0, 99},
                    bridge_documents::Position{0, 99}
                },
                bridge_documents::utf8_to_utf16("")
            }
        });

    const auto* document = manager.find_document("file:///c:/repo/doc.txt");
    assert(document != nullptr);
    assert(document->getVersion() == 1);
    assert(document->getLines().size() == 2);
    assert(bridge_documents::utf16_to_utf8(document->getLine(0)) == "short");
    assert(bridge_documents::utf16_to_utf8(document->getLine(1)) == "text");
}

DEFINE_TEST(test_response_manager_round_trip) {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId original_id = std::string{"req-1"};
    const RequestId bridge_id = manager->create_bridge_request_id(original_id, ioc.get_executor());

    std::optional<ResponseManager::WaitResult> result;

    net::co_spawn(ioc,
        [manager, &result, bridge_id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(bridge_id);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager, bridge_id]() {
        manager->store_response(bridge_id, json{{"jsonrpc", "2.0"}, {"id", jsonrpc::request_id_to_json(bridge_id)}, {"result", json{{"ok", true}}}});
    });

    ioc.run();

    assert(result.has_value());
    assert(result->status == ResponseManager::WaitStatus::response_ready);
    assert(result->response.has_value());
    assert((*result->response)["id"] == "req-1");
    assert((*result->response)["result"]["ok"] == true);
}

DEFINE_TEST(test_response_manager_integer_id_round_trip) {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId original_id = std::int64_t{17};
    const RequestId bridge_id = manager->create_bridge_request_id(original_id, ioc.get_executor());

    std::optional<ResponseManager::WaitResult> result;

    net::co_spawn(ioc,
        [manager, &result, bridge_id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(bridge_id);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager, bridge_id]() {
        manager->store_response(bridge_id, json{{"jsonrpc", "2.0"}, {"id", jsonrpc::request_id_to_json(bridge_id)}, {"result", json{{"ok", true}}}});
    });

    ioc.run();

    assert(result.has_value());
    assert(result->status == ResponseManager::WaitStatus::response_ready);
    assert(result->response.has_value());
    assert((*result->response)["id"] == 17);
}

DEFINE_TEST(test_response_manager_allows_duplicate_original_ids) {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId original_id = std::int64_t{42};
    const RequestId bridge_id_a = manager->create_bridge_request_id(original_id, ioc.get_executor());
    const RequestId bridge_id_b = manager->create_bridge_request_id(original_id, ioc.get_executor());

    assert(bridge_id_a != bridge_id_b);

    std::optional<ResponseManager::WaitResult> result_a;
    std::optional<ResponseManager::WaitResult> result_b;

    net::co_spawn(ioc,
        [manager, &result_a, bridge_id_a]() -> net::awaitable<void> {
            result_a = co_await manager->wait_for_response(bridge_id_a);
            co_return;
        },
        net::detached);

    net::co_spawn(ioc,
        [manager, &result_b, bridge_id_b]() -> net::awaitable<void> {
            result_b = co_await manager->wait_for_response(bridge_id_b);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager, bridge_id_b]() {
        manager->store_response(bridge_id_b, json{{"jsonrpc", "2.0"}, {"id", jsonrpc::request_id_to_json(bridge_id_b)}, {"result", json{{"which", "b"}}}});
    });

    net::post(ioc, [manager, bridge_id_a]() {
        manager->store_response(bridge_id_a, json{{"jsonrpc", "2.0"}, {"id", jsonrpc::request_id_to_json(bridge_id_a)}, {"result", json{{"which", "a"}}}});
    });

    ioc.run();

    assert(result_a.has_value());
    assert(result_b.has_value());
    assert(result_a->status == ResponseManager::WaitStatus::response_ready);
    assert(result_b->status == ResponseManager::WaitStatus::response_ready);
    assert(result_a->response.has_value());
    assert(result_b->response.has_value());
    assert((*result_a->response)["id"] == 42);
    assert((*result_b->response)["id"] == 42);
    assert((*result_a->response)["result"]["which"] == "a");
    assert((*result_b->response)["result"]["which"] == "b");
}

DEFINE_TEST(test_response_manager_rejects_null_original_ids) {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    bool threw = false;

    try {
        static_cast<void>(manager->create_bridge_request_id(RequestId{std::monostate{}}, ioc.get_executor()));
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
}

DEFINE_TEST(test_response_manager_cancel_all_releases_waiters) {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId id = std::string{"req-cancel"};
    const RequestId bridge_id = manager->create_bridge_request_id(id, ioc.get_executor());

    std::optional<ResponseManager::WaitResult> result;

    net::co_spawn(ioc,
        [manager, &result, bridge_id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(bridge_id);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager]() {
        manager->cancel_all();
    });

    ioc.run();

    assert(result.has_value());
    assert(result->status == ResponseManager::WaitStatus::cancelled);
    assert(!result->response.has_value());
}

DEFINE_TEST(test_response_manager_times_out_waiters) {
    using namespace std::chrono_literals;

    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId id = std::string{"req-timeout"};
    const RequestId bridge_id = manager->create_bridge_request_id(id, ioc.get_executor(), 20ms);

    std::optional<ResponseManager::WaitResult> result;

    net::co_spawn(ioc,
        [manager, &result, bridge_id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(bridge_id);
            co_return;
        },
        net::detached);

    ioc.run();

    assert(result.has_value());
    assert(result->status == ResponseManager::WaitStatus::timed_out);
    assert(!result->response.has_value());
}

DEFINE_TEST(test_try_store_jsonrpc_response_matches_pending_request) {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId original_id = std::string{"req-local-response"};
    const RequestId bridge_id = manager->create_bridge_request_id(original_id, ioc.get_executor());

    std::optional<ResponseManager::WaitResult> result;

    net::co_spawn(ioc,
        [manager, &result, bridge_id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(bridge_id);
            co_return;
        },
        net::detached);

    const bool stored = bridge_local::try_store_jsonrpc_response(
        json{{"jsonrpc", "2.0"}, {"id", jsonrpc::request_id_to_json(bridge_id)}, {"result", json{{"ok", true}}}},
        manager);

    ioc.run();

    assert(stored);
    assert(result.has_value());
    assert(result->status == ResponseManager::WaitStatus::response_ready);
    assert(result->response.has_value());
    assert((*result->response)["id"] == "req-local-response");
    assert((*result->response)["result"]["ok"] == true);
}

DEFINE_TEST(test_local_request_dispatcher_round_trips_over_local_session) {
    using namespace std::chrono_literals;

    net::io_context ioc;

    tcp::acceptor acceptor(ioc, {tcp::v4(), 0});
    tcp::socket client_socket(ioc);
    client_socket.connect({net::ip::address_v4::loopback(), acceptor.local_endpoint().port()});

    auto server_socket = std::make_shared<tcp::socket>(ioc);
    acceptor.accept(*server_socket);
    auto session = std::make_shared<LocalSession>(server_socket);
    auto response_manager = std::make_shared<ResponseManager>();
    auto dispatcher = std::make_shared<bridge_local::LocalRequestDispatcher>(session, response_manager, 100ms);

    beast::flat_buffer read_buffer;
    std::optional<json> outbound_request;
    std::optional<json> response;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            response = co_await dispatcher->send_request("dltxt/get_parser_regex");
            co_return;
        },
        net::detached);

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            auto payload = co_await lsp::read_message(client_socket, read_buffer);
            assert(payload.has_value());
            outbound_request = json::parse(*payload);
            assert((*outbound_request)["method"] == "dltxt/get_parser_regex");
            assert((*outbound_request)["params"].is_object());

            const bool stored = bridge_local::try_store_jsonrpc_response(
                json{
                    {"jsonrpc", "2.0"},
                    {"id", (*outbound_request)["id"]},
                    {"result", json{
                        {"originalPrefixRegex", "JP"},
                        {"translatedPrefixRegex", "CN"},
                        {"otherPrefixRegex", "OTHER"},
                        {"originalWhiteRegex", ""},
                        {"translatedWhiteRegex", ""},
                        {"originalSuffixRegex", ""},
                        {"translatedSuffixRegex", ""}
                    }}
                },
                response_manager);
            assert(stored);
            co_return;
        },
        net::detached);

    ioc.run();

    assert(outbound_request.has_value());
    assert(response.has_value());
    assert((*response)["result"]["originalPrefixRegex"] == "JP");
    assert((*response)["result"]["translatedPrefixRegex"] == "CN");
}

DEFINE_TEST(test_standard_text_parser_builds_from_runtime_regex_request) {
    net::io_context ioc;
    std::optional<std::vector<bridge_text::ParsedPair>> pairs;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            auto parser = co_await bridge_text::StandardTextParser::create_from_request_sender(
                [](const std::string& method, const bridge_text::json&) -> net::awaitable<bridge_text::json> {
                    assert(method == "dltxt/get_parser_regex");
                    co_return bridge_text::json{
                        {"jsonrpc", "2.0"},
                        {"id", "local-client-1"},
                        {"result", bridge_text::json{
                            {"originalPrefixRegex", "A"},
                            {"translatedPrefixRegex", "B"},
                            {"otherPrefixRegex", ""},
                            {"originalWhiteRegex", ""},
                            {"translatedWhiteRegex", ""},
                            {"originalSuffixRegex", ""},
                            {"translatedSuffixRegex", ""}
                        }}
                    };
                });

            pairs = parser.parse_paired_lines(u"A\u3053\u3093\u306B\u3061\u306F\nB\u4F60\u597D\n");
            co_return;
        },
        net::detached);

    ioc.run();

    assert(pairs.has_value());
    assert(pairs->size() == 1);
    assert((*pairs)[0].original.text == u"\u3053\u3093\u306B\u3061\u306F");
    assert((*pairs)[0].translated.text == u"\u4F60\u597D");
}

struct FakeRemote {
    int id = -1;
};

DEFINE_TEST(test_remote_retry_loop_retries_with_fresh_remote_instances) {
    net::io_context ioc;
    auto active_remote = std::make_shared<ActiveRemote<FakeRemote>>();
    std::vector<std::shared_ptr<FakeRemote>> created_remotes;
    std::vector<int> attempted_ids;
    int error_count = 0;

    net::co_spawn(ioc,
        run_remote_retry_loop(
            active_remote,
            [&created_remotes](boost::asio::any_io_executor) {
                auto remote = std::make_shared<FakeRemote>();
                remote->id = static_cast<int>(created_remotes.size());
                created_remotes.push_back(remote);
                return remote;
            },
            [&attempted_ids, active_remote](const std::shared_ptr<FakeRemote>& remote) -> net::awaitable<void> {
                assert(active_remote->get() == remote);
                attempted_ids.push_back(remote->id);

                if (remote->id == 0) {
                    throw std::runtime_error("disconnect");
                }

                co_return;
            },
            [&error_count](std::exception_ptr error) {
                ++error_count;

                try {
                    if (error) {
                        std::rethrow_exception(error);
                    }
                } catch (const std::runtime_error& e) {
                    assert(std::string(e.what()) == "disconnect");
                } catch (...) {
                    assert(false);
                }
            },
            2),
        net::detached);

    ioc.run();

    assert(created_remotes.size() == 2);
    assert(created_remotes[0] != created_remotes[1]);
    assert(attempted_ids.size() == 2);
    assert(attempted_ids[0] == 0);
    assert(attempted_ids[1] == 1);
    assert(error_count == 1);
    assert(!active_remote->get());
}

DEFINE_TEST(test_remote_retry_loop_cancels_pending_responses_after_disconnect) {
    net::io_context ioc;
    auto active_remote = std::make_shared<ActiveRemote<FakeRemote>>();
    auto response_manager = std::make_shared<ResponseManager>();
    const RequestId id = std::string{"req-disconnect"};

    std::optional<ResponseManager::WaitResult> result;
    int error_count = 0;

    const RequestId bridge_id = response_manager->create_bridge_request_id(id, ioc.get_executor());

    net::co_spawn(ioc,
        [response_manager, &result, bridge_id]() -> net::awaitable<void> {
            result = co_await response_manager->wait_for_response(bridge_id);
            co_return;
        },
        net::detached);

    net::co_spawn(ioc,
        run_remote_retry_loop(
            active_remote,
            [](boost::asio::any_io_executor) {
                return std::make_shared<FakeRemote>();
            },
            [response_manager](const std::shared_ptr<FakeRemote>&) -> net::awaitable<void> {
                response_manager->cancel_all();
                throw std::runtime_error("disconnect");
            },
            [&error_count](std::exception_ptr error) {
                ++error_count;

                try {
                    if (error) {
                        std::rethrow_exception(error);
                    }
                } catch (const std::runtime_error& e) {
                    assert(std::string(e.what()) == "disconnect");
                } catch (...) {
                    assert(false);
                }
            },
            1),
        net::detached);

    ioc.run();

    assert(error_count == 1);
    assert(result.has_value());
    assert(result->status == ResponseManager::WaitStatus::cancelled);
    assert(!active_remote->get());
}

DEFINE_TEST(test_remote_retry_loop_applies_backoff_after_failure) {
    using namespace std::chrono_literals;

    net::io_context ioc;
    auto active_remote = std::make_shared<ActiveRemote<FakeRemote>>();
    std::vector<std::chrono::steady_clock::time_point> attempt_times;

    net::co_spawn(ioc,
        run_remote_retry_loop(
            active_remote,
            [](boost::asio::any_io_executor) {
                return std::make_shared<FakeRemote>();
            },
            [&attempt_times](const std::shared_ptr<FakeRemote>&) -> net::awaitable<void> {
                attempt_times.push_back(std::chrono::steady_clock::now());
                if (attempt_times.size() == 1) {
                    throw std::runtime_error("disconnect");
                }

                co_return;
            },
            [](std::exception_ptr) {},
            2,
            20ms,
            40ms),
        net::detached);

    ioc.run();

    assert(attempt_times.size() == 2);
    assert((attempt_times[1] - attempt_times[0]) >= 15ms);
}

const std::vector<NamedTest>& all_tests() {
    return registered_tests();
}
}

int main(int argc, char* argv[]) {
    const auto& tests = all_tests();

    if (argc == 1) {
        for (const auto& test : tests) {
            test.fn();
        }
        return 0;
    }

    const std::string requested_test = argv[1];
    for (const auto& test : tests) {
        if (requested_test == test.name) {
            test.fn();
            return 0;
        }
    }

    std::fprintf(stderr, "Unknown test: %s\n", requested_test.c_str());
    return 1;
}