#pragma once

#include <boost/asio.hpp>

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
    std::size_t max_attempts = 0) {

    auto executor = co_await boost::asio::this_coro::executor;

    for (std::size_t attempt = 0; max_attempts == 0 || attempt < max_attempts; ++attempt) {
        auto remote = make_remote(executor);
        active_remote->replace(remote);

        try {
            co_await run_remote(remote);
        } catch (...) {
            on_error(std::current_exception());
        }

        active_remote->clear_if_current(remote);
    }
}