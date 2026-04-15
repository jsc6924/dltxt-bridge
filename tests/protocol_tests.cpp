#include <boost/asio.hpp>

#include <cassert>
#include <optional>
#include <string>

#include "../bridge_protocol.hpp"

namespace {
void test_frame_message() {
    const std::string payload = R"({"jsonrpc":"2.0","method":"initialize"})";
    const std::string framed = lsp::frame_message(payload);

    const std::string expected_prefix = "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    assert(framed.rfind(expected_prefix, 0) == 0);
    assert(framed.substr(expected_prefix.size()) == payload);
}

void test_parse_content_length_with_crlf() {
    const std::string headers = "Content-Length: 123\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n";
    const auto parsed = lsp::parse_content_length(headers);

    assert(parsed.has_value());
    assert(*parsed == 123U);
}

void test_parse_content_length_case_insensitive() {
    const std::string headers = "content-length: 42\r\n\r\n";
    const auto parsed = lsp::parse_content_length(headers);

    assert(parsed.has_value());
    assert(*parsed == 42U);
}

void test_parse_content_length_missing() {
    const auto parsed = lsp::parse_content_length("Content-Type: text/plain\r\n\r\n");
    assert(!parsed.has_value());
}

void test_jsonrpc_helpers() {
    const json response = jsonrpc::create_response(1, json{{"ok", true}});
    assert(response["jsonrpc"] == "2.0");
    assert(response["id"] == 1);
    assert(response["result"]["ok"] == true);

    const json error = jsonrpc::create_error(2, -32600, "Invalid Request");
    assert(error["error"]["code"] == -32600);
    assert(error["error"]["message"] == "Invalid Request");
}

void test_response_manager_round_trip() {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const json id = "req-1";
    manager->create_request_tracker(id, ioc.get_executor());

    std::optional<json> result;

    net::co_spawn(ioc,
        [manager, &result, id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(id);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager, id]() {
        manager->store_response(id, json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json{{"ok", true}}}});
    });

    ioc.run();

    assert(result.has_value());
    assert((*result)["result"]["ok"] == true);
}
}

int main() {
    test_frame_message();
    test_parse_content_length_with_crlf();
    test_parse_content_length_case_insensitive();
    test_parse_content_length_missing();
    test_jsonrpc_helpers();
    test_response_manager_round_trip();
    return 0;
}