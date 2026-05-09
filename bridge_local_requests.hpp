#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "bridge_protocol.hpp"

namespace bridge_local {

inline bool try_store_jsonrpc_response(const json& message, const std::shared_ptr<ResponseManager>& response_manager) {
    if (!response_manager || !jsonrpc::is_valid_jsonrpc_response(message)) {
        return false;
    }

    const auto maybe_id = jsonrpc::request_id_from_json(message["id"]);
    if (!maybe_id.has_value() || !jsonrpc::is_trackable_request_id(*maybe_id)) {
        return false;
    }

    return response_manager->store_response(*maybe_id, message);
}

class LocalRequestDispatcher {
    std::shared_ptr<LocalSession> session_;
    std::shared_ptr<ResponseManager> response_manager_;
    std::chrono::milliseconds request_timeout_;
    std::atomic<std::uint64_t> next_original_request_id_{0};

public:
    LocalRequestDispatcher(
        std::shared_ptr<LocalSession> session,
        std::shared_ptr<ResponseManager> response_manager,
        std::chrono::milliseconds request_timeout)
        : session_(std::move(session)),
          response_manager_(std::move(response_manager)),
          request_timeout_(request_timeout) {}

    net::awaitable<json> send_request(const std::string& method, const json& params = json::object()) {
        if (!session_) {
            throw std::invalid_argument("Local request dispatcher requires a session");
        }

        if (!response_manager_) {
            throw std::invalid_argument("Local request dispatcher requires a response manager");
        }

        const auto executor = co_await net::this_coro::executor;
        const auto original_request_number = next_original_request_id_.fetch_add(1) + 1;
        const RequestId original_id = std::string{"local-client-" + std::to_string(original_request_number)};
        const RequestId bridge_id = response_manager_->create_bridge_request_id(original_id, executor, request_timeout_);
        ResponseCleanup cleanup{response_manager_, bridge_id};

        const json request = jsonrpc::create_request(
            jsonrpc::request_id_to_json(bridge_id),
            method,
            params);

        co_await session_->write_framed(lsp::frame_message(request.dump()));

        const auto wait_result = co_await response_manager_->wait_for_response(bridge_id);
        if (wait_result.status == ResponseManager::WaitStatus::response_ready && wait_result.response.has_value()) {
            co_return *wait_result.response;
        }

        if (wait_result.status == ResponseManager::WaitStatus::timed_out) {
            throw std::runtime_error("Local client request timed out");
        }

        throw std::runtime_error("Local client request was cancelled");
    }
};

}  // namespace bridge_local
