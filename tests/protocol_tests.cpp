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
#include "../bridge_crossref.hpp"
#include "../bridge_documents.hpp"
#include "../batch.hpp"
#include "../bridge_local_requests.hpp"
#include "../bridge_protocol.hpp"
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

DEFINE_TEST(test_local_handler_rejects_simpletm_requests) {
    const json request = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "simpletm/complete"}};
    const auto handling = bridge_local::handle_request(nullptr, request);

    assert(!handling.forward_to_remote);
    assert(handling.response.has_value());
    assert((*handling.response)["error"]["code"] == -32601);
    assert((*handling.response)["error"]["message"] == "method simpletm/complete is not recognized");
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

DEFINE_TEST(test_shared_thread_pool_defaults_to_at_least_two_threads) {
    bridge_runtime::shutdown_shared_thread_pool();
    bridge_runtime::initialize_shared_thread_pool(1);

    std::atomic<int> counter = 0;
    std::latch done(2);
    boost::asio::post(bridge_runtime::shared_thread_pool(), [&]() {
        ++counter;
        done.count_down();
    });
    boost::asio::post(bridge_runtime::shared_thread_pool(), [&]() {
        ++counter;
        done.count_down();
    });
    done.wait();

    assert(counter == 2);

    bridge_runtime::shutdown_shared_thread_pool();
}

DEFINE_TEST(test_utf_conversions_accept_empty_strings) {
    assert(bridge_documents::utf8_to_utf16("").empty());
    assert(bridge_documents::utf16_to_utf8(std::u16string{}).empty());
}

DEFINE_TEST(test_utf_conversions_preserve_japanese_quotes_and_markers) {
    const std::u16string original = u"\u2605txt00641\u2605\u300C\u308F\u3001\u308F\u3041\u3042\u3063\uFF01\uFF1F\u300D";

    const std::string utf8 = bridge_documents::utf16_to_utf8(original);
    const std::u16string round_tripped = bridge_documents::utf8_to_utf16(utf8);

    assert(round_tripped == original);
}

DEFINE_TEST(test_file_uri_round_trips_japanese_filename_on_windows_safe_paths) {
    const auto workspace_path = std::filesystem::temp_directory_path() / u8"dltxt_bridge_\u65E5\u672C\u8A9E";
    std::filesystem::create_directories(workspace_path);
    const auto file_path = workspace_path / u8"\u674F\u73E0\u221A209_11\u6708_02.ks.json.txt";
    {
        std::ofstream output(file_path, std::ios::binary);
        output << "jp-path";
    }

    const std::string uri = bridge_documents::file_uri_from_path(file_path);
    const std::filesystem::path round_tripped = bridge_documents::file_path_from_uri(uri);
    const auto document = bridge_batch::load_document_fast_from_uri(uri);

    assert(round_tripped == file_path);
    assert(document.getFilePath() == file_path);
    assert(bridge_documents::utf16_to_utf8(document.getContent()) == "jp-path");

    std::filesystem::remove(file_path);
    std::filesystem::remove(workspace_path);
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

DEFINE_TEST(test_similarity_index_prefers_exact_match_with_many_common_candidates) {
    bridge_similarity::SimilarityIndex index;

    for (std::size_t i = 0; i < 600; ++i) {
        std::u16string text = u"common-prefix-abcdefghijklmnop-";
        text += static_cast<char16_t>(u'a' + static_cast<char16_t>(i % 26));
        text += static_cast<char16_t>(u'a' + static_cast<char16_t>((i / 26) % 26));
        text += static_cast<char16_t>(u'a' + static_cast<char16_t>((i / (26 * 26)) % 26));
        index.upsert(1000 + i, "c:/repo/common-" + std::to_string(i) + ".txt", i, text);
    }

    const std::u16string exact = u"common-prefix-abcdefghijklmnop-xyz";
    index.upsert(9999, "c:/repo/exact.txt", 42, exact);

    const auto results = index.search(exact, 0.0, 5);

    assert(!results.empty());
    assert(results[0].line_id == 9999);
    assert(results[0].file_path == "c:/repo/exact.txt");
    assert(results[0].line_number == 42);
}

DEFINE_TEST(test_crossref_service_returns_bridge_matches_for_open_documents) {
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

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_workspace";
    std::filesystem::create_directories(workspace_path);
    const auto current_file_path = workspace_path / "current.txt";
    const auto other_file_path = workspace_path / "other.txt";

    {
        std::ofstream output(current_file_path, std::ios::binary);
        output << white_circle << "0001T" << white_circle << "old\n"
               << black_circle << "0001T" << black_circle << "old\n";
    }
    {
        std::ofstream output(other_file_path, std::ios::binary);
        output << white_circle << "0002T" << white_circle << "miss\n"
               << black_circle << "0002T" << black_circle << "miss\n";
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    const std::string other_uri = bridge_documents::file_uri_from_path(other_file_path);
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0001T" + black_circle + "hello\n",
        3);
    manager.open_from_lsp(
        other_uri,
        white_circle + "0002T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0002T" + black_circle + "world\n",
        4);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-open-docs"};
    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["lineNumber"] == 0);
    assert(result["matches"][0]["exactCount"] == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["fileName"] == other_file_path.string());
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == "world");
    assert(result["matches"][0]["refs"][0]["score"] == 100);

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_returns_bridge_matches_for_text_block_parser) {
    const std::string white_circle = utf8_bytes("\xE2\x97\x8B");
    const std::string black_circle = utf8_bytes("\xE2\x97\x8F");
    const bridge_text::TextBlockConfig config{
        "\\[BLOCK\\]\\r?\\n(?<jp>" + white_circle + "\\d+[TN]" + white_circle + ".*)\\r?\\n(?<cn>" + black_circle + "\\d+[TN]" + black_circle + ".*)\\r?\\n\\[/BLOCK\\]",
        white_circle + "\\d+[TN]" + white_circle,
        black_circle + "\\d+[TN]" + black_circle,
        "",
        "",
        "",
        "",
    };

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_text_block_workspace";
    std::filesystem::create_directories(workspace_path);
    const auto current_file_path = workspace_path / "current.txt";
    const auto other_file_path = workspace_path / "other.txt";

    {
        std::ofstream output(current_file_path, std::ios::binary);
        output << "[BLOCK]\n"
               << white_circle << "0001T" << white_circle << "old\n"
               << black_circle << "0001T" << black_circle << "old\n"
               << "[/BLOCK]\n";
    }
    {
        std::ofstream output(other_file_path, std::ios::binary);
        output << "[BLOCK]\n"
               << white_circle << "0002T" << white_circle << "miss\n"
               << black_circle << "0002T" << black_circle << "miss\n"
               << "[/BLOCK]\n";
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    const std::string other_uri = bridge_documents::file_uri_from_path(other_file_path);
    manager.open_from_lsp(
        current_uri,
        "[BLOCK]\n"
            + white_circle + "0001T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0001T" + black_circle + "hello\n"
            + "[/BLOCK]\n",
        3);
    manager.open_from_lsp(
        other_uri,
        "[BLOCK]\n"
            + white_circle + "0002T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0002T" + black_circle + "world\n"
            + "[/BLOCK]\n",
        4);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-text-block-open-docs"};
    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["lineNumber"] == 1);
    assert(result["matches"][0]["exactCount"] == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["fileName"] == other_file_path.string());
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == "world");
    assert(result["matches"][0]["refs"][0]["score"] == 100);

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_preserves_japanese_quotes_in_json_results) {
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

    const std::string quoted_jp = bridge_documents::utf16_to_utf8(u"\u300C\u308F\u3001\u308F\u3041\u3042\u3063\uFF01\uFF1F\u300D");
    const std::string translated = bridge_documents::utf16_to_utf8(u"\u300C\u54C7\u3001\u54C7\u554A\u554A\uFF01\uFF1F\u300D");

    bridge_documents::DocumentManager manager;
    const std::string current_uri = "file:///c:/tmp/current.txt";
    const std::string other_uri = "file:///c:/tmp/other.txt";
    manager.set_workspace_folders({"file:///c:/tmp"});
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + quoted_jp + "\n"
            + black_circle + "0001T" + black_circle + translated + "\n",
        1);
    manager.open_from_lsp(
        other_uri,
        white_circle + "0002T" + white_circle + quoted_jp + "\n"
            + black_circle + "0002T" + black_circle + translated + "\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-preserve-quotes"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["jpLine"] == quoted_jp);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == translated);
}

DEFINE_TEST(test_crossref_service_preserves_japanese_quotes_in_disk_loaded_json_results) {
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

    const std::string quoted_jp = bridge_documents::utf16_to_utf8(u"\u300C\u308F\u3001\u308F\u3041\u3042\u3063\uFF01\uFF1F\u300D");
    const std::string translated = bridge_documents::utf16_to_utf8(u"\u300C\u54C7\u3001\u54C7\u554A\u554A\uFF01\uFF1F\u300D");

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_disk_utf16_workspace";
    std::filesystem::create_directories(workspace_path);
    const auto other_file_path = workspace_path / "other.txt";

    {
        const std::u16string disk_text = bridge_documents::utf8_to_utf16(
            white_circle + "0002T" + white_circle + quoted_jp + "\r\n"
                + black_circle + "0002T" + black_circle + translated + "\r\n");
        std::ofstream output(other_file_path, std::ios::binary);
        const unsigned char bom[] = {0xFF, 0xFE};
        output.write(reinterpret_cast<const char*>(bom), static_cast<std::streamsize>(sizeof(bom)));
        for (char16_t ch : disk_text) {
            const unsigned char lo = static_cast<unsigned char>(ch & 0x00FFu);
            const unsigned char hi = static_cast<unsigned char>((ch >> 8u) & 0x00FFu);
            output.put(static_cast<char>(lo));
            output.put(static_cast<char>(hi));
        }
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});
    const std::string current_uri = bridge_documents::file_uri_from_path(workspace_path / "current.txt");
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + quoted_jp + "\n"
            + black_circle + "0001T" + black_circle + translated + "\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-preserve-quotes-disk"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["fileName"] == other_file_path.string());
    assert(result["matches"][0]["refs"][0]["lineInfo"]["jpLine"] == quoted_jp);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == translated);

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_ignores_non_file_like_current_uri) {
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

    bridge_crossref::CrossrefService service;
    bridge_documents::TextDocument document(
        "output:extension-output-jsc723.translateassistant-%233-DLTXT%20Bridge%20stderr",
        "client-decoded",
        "output:extension-output-jsc723.translateassistant-%233-DLTXT%20Bridge%20stderr",
        1,
        bridge_documents::utf8_to_utf16("ignored"));

    const json result = service.search_json(document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].empty());
}

DEFINE_TEST(test_crossref_service_skips_lines_when_exact_count_reaches_limit) {
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

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_exact_limit";
    std::filesystem::create_directories(workspace_path);

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string repeated_text = utf8_bytes("\xE5\xBF\x83\xE3\x81\xAE\xE5\xA3\xB0");
    for (int index = 0; index < 10; ++index) {
        const auto file_path = workspace_path / ("other-" + std::to_string(index) + ".txt");
        std::ofstream output(file_path, std::ios::binary);
        output << white_circle << "0001T" << white_circle << repeated_text << "\n"
               << black_circle << "0001T" << black_circle << repeated_text << "\n";
    }

    const auto current_file_path = workspace_path / "current.txt";
    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + repeated_text + "\n"
            + black_circle + "0001T" + black_circle + repeated_text + "\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-exact-limit"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].empty());

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_skips_gitignored_paths_during_initial_scan) {
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

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_gitignore_workspace";
    const auto ignored_dir_path = workspace_path / "ignored";
    std::filesystem::create_directories(ignored_dir_path);

    const auto current_file_path = workspace_path / "current.txt";
    const auto included_file_path = workspace_path / "included.txt";
    const auto ignored_file_path = ignored_dir_path / "ignored.txt";

    {
        std::ofstream output(workspace_path / ".gitignore", std::ios::binary);
        output << "ignored/\n";
    }
    {
        std::ofstream output(included_file_path, std::ios::binary);
        output << white_circle << "0002T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0002T" << black_circle << "included\n";
    }
    {
        std::ofstream output(ignored_file_path, std::ios::binary);
        output << white_circle << "0003T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0003T" << black_circle << "ignored\n";
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0001T" + black_circle + "current\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-gitignore-initial-scan"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["fileName"] == included_file_path.string());
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == "included");

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_skips_paths_from_dltxt_ignore_during_initial_scan) {
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

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_dltxt_ignore_workspace";
    const auto ignored_dir_path = workspace_path / "ignored";
    const auto dltxt_dir_path = workspace_path / ".dltxt";
    std::filesystem::create_directories(ignored_dir_path);
    std::filesystem::create_directories(dltxt_dir_path);

    const auto current_file_path = workspace_path / "current.txt";
    const auto included_file_path = workspace_path / "included.txt";
    const auto ignored_file_path = ignored_dir_path / "ignored.txt";

    {
        std::ofstream output(dltxt_dir_path / "ignore", std::ios::binary);
        output << "ignored/\n";
    }
    {
        std::ofstream output(included_file_path, std::ios::binary);
        output << white_circle << "0002T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0002T" << black_circle << "included\n";
    }
    {
        std::ofstream output(ignored_file_path, std::ios::binary);
        output << white_circle << "0003T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0003T" << black_circle << "ignored\n";
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0001T" + black_circle + "current\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-dltxt-ignore-initial-scan"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["fileName"] == included_file_path.string());
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == "included");

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_ignores_non_text_dltxt_ignore_file) {
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

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_dltxt_ignore_binary_workspace";
    const auto ignored_dir_path = workspace_path / "ignored";
    const auto dltxt_dir_path = workspace_path / ".dltxt";
    std::filesystem::create_directories(ignored_dir_path);
    std::filesystem::create_directories(dltxt_dir_path);

    const auto current_file_path = workspace_path / "current.txt";
    const auto included_file_path = workspace_path / "included.txt";
    const auto ignored_file_path = ignored_dir_path / "ignored.txt";

    {
        std::ofstream output(dltxt_dir_path / "ignore", std::ios::binary);
        const char bytes[] = {'i', 'g', 'n', 'o', 'r', 'e', 'd', '/', '\0', '\n'};
        output.write(bytes, static_cast<std::streamsize>(sizeof(bytes)));
    }
    {
        std::ofstream output(included_file_path, std::ios::binary);
        output << white_circle << "0002T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0002T" << black_circle << "included\n";
    }
    {
        std::ofstream output(ignored_file_path, std::ios::binary);
        output << white_circle << "0003T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0003T" << black_circle << "ignored\n";
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0001T" + black_circle + "current\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-dltxt-ignore-binary"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["refs"].size() == 2);

    bool found_included = false;
    bool found_ignored = false;
    for (const auto& ref : result["matches"][0]["refs"]) {
        const std::string file_name = ref["lineInfo"]["fileName"].get<std::string>();
        if (file_name == included_file_path.string()) {
            found_included = true;
        }
        if (file_name == ignored_file_path.string()) {
            found_ignored = true;
        }
    }
    assert(found_included);
    assert(found_ignored);

    std::filesystem::remove_all(workspace_path);
}

DEFINE_TEST(test_crossref_service_skips_unicode_paths_from_dltxt_ignore_during_initial_scan) {
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

    const auto workspace_path = std::filesystem::temp_directory_path() / "dltxt_bridge_crossref_dltxt_ignore_unicode_workspace";
    const auto ignored_dir_name = std::filesystem::path(bridge_documents::utf8_to_utf16("\xE5\x8E\x9F\xE6\x96\x87"));
    const auto ignored_dir_path = workspace_path / ignored_dir_name;
    const auto dltxt_dir_path = workspace_path / ".dltxt";
    std::filesystem::create_directories(ignored_dir_path);
    std::filesystem::create_directories(dltxt_dir_path);

    const auto current_file_path = workspace_path / "current.txt";
    const auto included_file_path = workspace_path / "included.txt";
    const auto ignored_file_path = ignored_dir_path / "ignored.txt";

    {
        std::ofstream output(dltxt_dir_path / "ignore", std::ios::binary);
        output << "\xE5\x8E\x9F\xE6\x96\x87/\n";
    }
    {
        std::ofstream output(included_file_path, std::ios::binary);
        output << white_circle << "0002T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0002T" << black_circle << "included\n";
    }
    {
        std::ofstream output(ignored_file_path, std::ios::binary);
        output << white_circle << "0003T" << white_circle << utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") << "\n"
               << black_circle << "0003T" << black_circle << "ignored\n";
    }

    bridge_documents::DocumentManager manager;
    manager.set_workspace_folders({bridge_documents::file_uri_from_path(workspace_path)});

    const std::string current_uri = bridge_documents::file_uri_from_path(current_file_path);
    manager.open_from_lsp(
        current_uri,
        white_circle + "0001T" + white_circle + utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF") + "\n"
            + black_circle + "0001T" + black_circle + "current\n",
        1);

    bridge_crossref::CrossrefService service;
    boost::asio::thread_pool pool(2);
    const bridge_crossref::ParserState parser_state{config, "test-crossref-dltxt-ignore-unicode"};

    service.update_parser_config(
        parser_state,
        manager.workspace_folders_snapshot(),
        manager.open_documents_snapshot(),
        pool);
    pool.join();

    const auto current_document = manager.document_snapshot(current_uri);
    assert(current_document.has_value());
    const json result = service.search_json(*current_document, 80, 10);

    assert(result.contains("matches"));
    assert(result["matches"].is_array());
    assert(result["matches"].size() == 1);
    assert(result["matches"][0]["refs"].size() == 1);
    assert(result["matches"][0]["refs"][0]["lineInfo"]["fileName"] == included_file_path.string());
    assert(result["matches"][0]["refs"][0]["lineInfo"]["trLine"] == "included");

    std::filesystem::remove_all(workspace_path);
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

DEFINE_TEST(test_standard_text_parser_preserves_multibyte_quote_regex_groups) {
    const std::string white_circle = utf8_bytes("\xE2\x97\x8B");
    const std::string black_circle = utf8_bytes("\xE2\x97\x8F");
    const bridge_text::RegexConfig config{
        white_circle + "\\d+[TN]" + white_circle,
        black_circle + "\\d+[TN]" + black_circle,
        "",
        "[「]?",
        "[“]?",
        "[」]?",
        "[”]?",
    };
    const bridge_text::StandardTextParser parser(config);

    const std::u16string text =
        u"\u25CB00027147T\u25CB\u300C\u3048\uFF1F\u3000\u3042\u3001\u3044\u3084\u3063\u300D\n"
        u"\u25CF00027147T\u25CF\u201C\u54C7\uFF1F\u3000\u554A\u3001\u4E0D\u662F\u201D\n";

    const auto pairs = parser.parse_paired_lines(text);

    assert(pairs.size() == 1);
    assert(pairs[0].original.white == u"\u300C");
    assert(pairs[0].original.text == u"\u3048\uFF1F\u3000\u3042\u3001\u3044\u3084\u3063");
    assert(pairs[0].original.suffix == u"\u300D");
    assert(pairs[0].translated.white == u"\u201C");
    assert(pairs[0].translated.text == u"\u54C7\uFF1F\u3000\u554A\u3001\u4E0D\u662F");
    assert(pairs[0].translated.suffix == u"\u201D");
}

DEFINE_TEST(test_standard_text_parser_skips_dangling_original_line_without_throwing) {
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

    const auto pairs = parser.parse_paired_lines(
        u"\u25CB00027145T\u25CB\u3053\u3093\u306B\u3061\u306F\u3002\n"
        u"\u25CF00027145T\u25CF\u4F60\u597D\u3002\n"
        u"\u25CB00027146T\u25CB\u3055\u3088\u3046\u306A\u3089\u3002\n");

    assert(pairs.size() == 1);
    assert(pairs[0].original.text == u"\u3053\u3093\u306B\u3061\u306F\u3002");
    assert(pairs[0].translated.text == u"\u4F60\u597D\u3002");
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
                        {"parserType", "standard"},
                        {"parserConfig", json{
                            {"originalPrefixRegex", "JP"},
                            {"translatedPrefixRegex", "CN"},
                            {"otherPrefixRegex", "OTHER"},
                            {"originalWhiteRegex", ""},
                            {"translatedWhiteRegex", ""},
                            {"originalSuffixRegex", ""},
                            {"translatedSuffixRegex", ""}
                        }}
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
    assert((*response)["result"]["parserType"] == "standard");
    assert((*response)["result"]["parserConfig"]["originalPrefixRegex"] == "JP");
    assert((*response)["result"]["parserConfig"]["translatedPrefixRegex"] == "CN");
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
                            {"parserType", "standard"},
                            {"parserConfig", bridge_text::json{
                                {"originalPrefixRegex", "A"},
                                {"translatedPrefixRegex", "B"},
                                {"otherPrefixRegex", ""},
                                {"originalWhiteRegex", ""},
                                {"translatedWhiteRegex", ""},
                                {"originalSuffixRegex", ""},
                                {"translatedSuffixRegex", ""}
                            }}
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

DEFINE_TEST(test_runtime_parser_factory_builds_text_block_parser_from_runtime_request) {
    net::io_context ioc;
    std::optional<std::vector<bridge_text::ParsedPair>> pairs;

    net::co_spawn(ioc,
        [&]() -> net::awaitable<void> {
            auto parser = co_await bridge_text::create_text_parser_from_request_sender(
                [](const std::string& method, const bridge_text::json&) -> net::awaitable<bridge_text::json> {
                    assert(method == "dltxt/get_parser_regex");
                    co_return bridge_text::json{
                        {"jsonrpc", "2.0"},
                        {"id", "local-client-2"},
                        {"result", bridge_text::json{
                            {"parserType", "text-block"},
                            {"parserConfig", bridge_text::json{
                                {"pattern", "\\[BLOCK\\]\\r?\\n(?<jp>A.*)\\r?\\n(?<cn>B.*)\\r?\\n\\[/BLOCK\\]"},
                                {"originalPrefixRegex", "A"},
                                {"translatedPrefixRegex", "B"},
                                {"originalWhiteRegex", "\\s*"},
                                {"translatedWhiteRegex", "\\s*"},
                                {"originalSuffixRegex", ""},
                                {"translatedSuffixRegex", ""}
                            }}
                        }}
                    };
                });

            pairs = parser->parse_paired_lines(u"[BLOCK]\nA  \u3053\u3093\u306B\u3061\u306F\nB  \u4F60\u597D\n[/BLOCK]\n");
            co_return;
        },
        net::detached);

    ioc.run();

    assert(pairs.has_value());
    assert(pairs->size() == 1);
    assert((*pairs)[0].original_line_index == 1);
    assert((*pairs)[0].translated_line_index == 2);
    assert((*pairs)[0].original.text == u"\u3053\u3093\u306B\u3061\u306F");
    assert((*pairs)[0].translated.text == u"\u4F60\u597D");
    assert((*pairs)[0].original.white == u"  ");
    assert((*pairs)[0].translated.white == u"  ");
}

DEFINE_TEST(test_text_block_parser_supports_multiline_anchors_like_extension_regex) {
    const bridge_text::TextBlockConfig config{
        "^-+\\d+-+(\\r)?\\n((\\*+)|(\\[[^\\]]*\\]))(\\r)?\\n(?<jp>.*)(\\r)?\\n=+(\\r)?\\n(?<cn>.*)((\\r)?\\n)*$",
        "",
        "",
        "",
        "",
        "[」』）]?",
        "[」』）]?",
    };
    const bridge_text::TextBlockParser parser(config);

    const auto pairs = parser.parse_paired_lines(bridge_documents::utf8_to_utf16(
        "-----------------------------0001-----------------------------\n"
        "**\n"
        "jp-1\n"
        "=========\n"
        "cn-1\n"
        "-----------------------------0002-----------------------------\n"
        "**\n"
        "jp-2\n"
        "=========\n"
        "cn-2\n"));

    assert(pairs.size() == 2);
    assert(pairs[0].original_line_index == 2);
    assert(pairs[0].translated_line_index == 4);
    assert(pairs[0].original.text == u"jp-1");
    assert(pairs[0].translated.text == u"cn-1");
    assert(pairs[1].original_line_index == 7);
    assert(pairs[1].translated_line_index == 9);
    assert(pairs[1].original.text == u"jp-2");
    assert(pairs[1].translated.text == u"cn-2");
}

DEFINE_TEST(test_standard_text_parser_parses_regex_with_extra_user_captures) {
    const std::string white_star = utf8_bytes("\xE2\x98\x86");
    const std::string black_star = utf8_bytes("\xE2\x98\x85");
    const std::string left_quote = utf8_bytes("\xE3\x80\x8C");
    const std::string right_quote = utf8_bytes("\xE3\x80\x8D");
    const std::string jp_hello = utf8_bytes("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF");
    const std::string zh_hello = utf8_bytes("\xE4\xBD\xA0\xE5\xA5\xBD");

    const bridge_text::RegexConfig config{
        white_star + "[A-Za-z0-9]+" + white_star + "(%n;)?",
        black_star + "[A-Za-z0-9]+" + black_star + "(%n;)?",
        "",
        "\\s*(" + left_quote + ")?",
        "\\s*(" + left_quote + ")?",
        "(" + right_quote + ")?",
        "(" + right_quote + ")?",
    };
    const bridge_text::StandardTextParser parser(config);

    const auto pairs = parser.parse_paired_lines(
        bridge_documents::utf8_to_utf16(
            white_star + "A001" + white_star + left_quote + jp_hello + right_quote + "\n"
                + black_star + "A001" + black_star + left_quote + zh_hello + right_quote + "\n"));

    assert(pairs.size() == 1);
    assert(bridge_documents::utf16_to_utf8(pairs[0].original.text) == jp_hello);
    assert(bridge_documents::utf16_to_utf8(pairs[0].translated.text) == zh_hello);
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