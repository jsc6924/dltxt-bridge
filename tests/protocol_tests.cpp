#include <boost/asio.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../bridge_app.hpp"
#include "../bridge_protocol.hpp"
#include "../bridge_runtime.hpp"

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

DEFINE_TEST(test_parse_bridge_settings_defaults) {
    const BridgeSettings settings = parse_bridge_settings({});

    assert(settings.local_port == 6009);
    assert(settings.remote_host == "127.0.0.1");
    assert(settings.remote_port == "9000");
    assert(settings.request_timeout == std::chrono::seconds(30));
}

DEFINE_TEST(test_parse_bridge_settings_overrides) {
    const BridgeSettings settings = parse_bridge_settings({
        "--listen-port", "6010",
        "--remote-host", "example.com",
        "--remote-port", "9010",
        "--request-timeout-ms", "1500"
    });

    assert(settings.local_port == 6010);
    assert(settings.remote_host == "example.com");
    assert(settings.remote_port == "9010");
    assert(settings.request_timeout == std::chrono::milliseconds(1500));
}

DEFINE_TEST(test_parse_bridge_settings_rejects_invalid_port) {
    bool threw = false;

    try {
        static_cast<void>(parse_bridge_settings({"--listen-port", "70000"}));
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

DEFINE_TEST(test_socket_helpers_identify_and_prune_dead_sockets) {
    net::io_context ioc;

    auto live_socket = std::make_shared<tcp::socket>(ioc);
    live_socket->open(tcp::v4());

    auto dead_socket = std::make_shared<tcp::socket>(ioc);
    std::vector<std::shared_ptr<tcp::socket>> sockets{live_socket, dead_socket};

    assert(!socket_is_dead(live_socket));
    assert(socket_is_dead(dead_socket));

    erase_sockets_by_identity(sockets, {dead_socket});

    assert(sockets.size() == 1);
    assert(sockets.front().get() == live_socket.get());
}

DEFINE_TEST(test_close_socket_if_open_closes_socket) {
    net::io_context ioc;
    auto socket = std::make_shared<tcp::socket>(ioc);
    socket->open(tcp::v4());

    assert(socket->is_open());

    close_socket_if_open(socket);

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