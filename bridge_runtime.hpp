#pragma once

#include <boost/asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>

template <typename Remote>
class ActiveRemote {
private:
    std::shared_ptr<Remote> current_;

public:
    void replace(std::shared_ptr<Remote> remote) {
        current_ = std::move(remote);
    }

    std::shared_ptr<Remote> get() const {
        return current_;
    }

    void clear_if_current(const std::shared_ptr<Remote>& remote) {
        if (current_.get() == remote.get()) {
            current_.reset();
        }
    }
};

template <typename Remote, typename MakeRemote, typename RunRemote, typename OnError>
boost::asio::awaitable<void> run_remote_retry_loop(
    const std::shared_ptr<ActiveRemote<Remote>>& active_remote,
    MakeRemote make_remote,
    RunRemote run_remote,
    OnError on_error,
    std::size_t max_attempts = 0,
    std::chrono::milliseconds initial_backoff = std::chrono::seconds(1),
    std::chrono::milliseconds max_backoff = std::chrono::seconds(30)) {

    auto executor = co_await boost::asio::this_coro::executor;
    auto backoff = initial_backoff;

    for (std::size_t attempt = 0; max_attempts == 0 || attempt < max_attempts; ++attempt) {
        auto remote = make_remote(executor);
        active_remote->replace(remote);

        bool failed = false;

        try {
            co_await run_remote(remote);
        } catch (...) {
            failed = true;
            on_error(std::current_exception());
        }

        active_remote->clear_if_current(remote);

        const bool has_more_attempts = max_attempts == 0 || (attempt + 1) < max_attempts;
        if (failed && has_more_attempts && backoff.count() > 0) {
            boost::asio::steady_timer timer(executor, backoff);
            co_await timer.async_wait(boost::asio::use_awaitable);
            backoff = std::min(max_backoff, backoff + backoff);
        } else if (!failed) {
            backoff = initial_backoff;
        }
    }
}