#include <boost/asio.hpp>

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../bridge_protocol.hpp"
#include "../bridge_runtime.hpp"

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

    const auto string_id = jsonrpc::request_id_from_json("abc");
    assert(string_id.has_value());
    assert(jsonrpc::request_id_to_json(*string_id) == "abc");

    const auto integer_id = jsonrpc::request_id_from_json(7);
    assert(integer_id.has_value());
    assert(jsonrpc::request_id_to_json(*integer_id) == 7);

    const auto invalid_id = jsonrpc::request_id_from_json(3.14);
    assert(!invalid_id.has_value());
}

void test_response_manager_round_trip() {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId id = std::string{"req-1"};
    manager->create_request_tracker(id, ioc.get_executor());

    std::optional<json> result;

    net::co_spawn(ioc,
        [manager, &result, id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(id);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager, id]() {
        manager->store_response(id, json{{"jsonrpc", "2.0"}, {"id", jsonrpc::request_id_to_json(id)}, {"result", json{{"ok", true}}}});
    });

    ioc.run();

    assert(result.has_value());
    assert((*result)["result"]["ok"] == true);
}

void test_response_manager_integer_id_round_trip() {
    net::io_context ioc;
    auto manager = std::make_shared<ResponseManager>();
    const RequestId id = std::int64_t{17};
    manager->create_request_tracker(id, ioc.get_executor());

    std::optional<json> result;

    net::co_spawn(ioc,
        [manager, &result, id]() -> net::awaitable<void> {
            result = co_await manager->wait_for_response(id);
            co_return;
        },
        net::detached);

    net::post(ioc, [manager, id]() {
        manager->store_response(id, json{{"jsonrpc", "2.0"}, {"id", 17}, {"result", json{{"ok", true}}}});
    });

    ioc.run();

    assert(result.has_value());
    assert((*result)["id"] == 17);
}

struct FakeRemote {
    int id = -1;
};

void test_remote_retry_loop_retries_with_fresh_remote_instances() {
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
}

int main() {
    test_frame_message();
    test_parse_content_length_with_crlf();
    test_parse_content_length_case_insensitive();
    test_parse_content_length_missing();
    test_jsonrpc_helpers();
    test_response_manager_round_trip();
    test_response_manager_integer_id_round_trip();
    test_remote_retry_loop_retries_with_fresh_remote_instances();
    return 0;
}