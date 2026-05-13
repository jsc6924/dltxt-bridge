#pragma once

#include <boost/asio.hpp>

#include <algorithm>
#include <csignal>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>
#include <deque>
#include <optional>

#include "local_session.hpp"
#include "bridge_protocol.hpp"

namespace net = boost::asio;

struct BridgeSettings {
    std::string remote_host = "127.0.0.1";
    std::string remote_port = "9000";
    std::chrono::milliseconds request_timeout = std::chrono::seconds(30);
    bool show_version = false;
};

inline std::vector<int> bridge_signal_numbers() {
    std::vector<int> signals{SIGINT, SIGTERM};
#ifdef SIGBREAK
    signals.push_back(SIGBREAK);
#endif
    return signals;
}

inline std::unique_ptr<net::signal_set> install_stop_signals(net::io_context& ioc) {
    auto signals = std::make_unique<net::signal_set>(ioc);
    for (int signal_number : bridge_signal_numbers()) {
        signals->add(signal_number);
    }

    signals->async_wait([&ioc](const boost::system::error_code&, int) {
        ioc.stop();
    });

    return signals;
}

inline std::chrono::milliseconds parse_timeout_ms(const std::string& value, const char* option_name) {
    try {
        const auto parsed = std::stoull(value);
        if (parsed == 0) {
            throw std::out_of_range("timeout out of range");
        }

        return std::chrono::milliseconds(parsed);
    } catch (...) {
        throw std::invalid_argument(std::string("Invalid value for ") + option_name + ": " + value);
    }
}

inline BridgeSettings parse_bridge_settings(const std::vector<std::string>& args) {
    BridgeSettings settings;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--remote-host") {
            if (index + 1 >= args.size()) {
                throw std::invalid_argument("Missing value for --remote-host");
            }

            settings.remote_host = args[++index];
            continue;
        }

        if (arg == "--remote-port") {
            if (index + 1 >= args.size()) {
                throw std::invalid_argument("Missing value for --remote-port");
            }

            settings.remote_port = args[++index];
            continue;
        }

        if (arg == "--request-timeout-ms") {
            if (index + 1 >= args.size()) {
                throw std::invalid_argument("Missing value for --request-timeout-ms");
            }

            settings.request_timeout = parse_timeout_ms(args[++index], "--request-timeout-ms");
            continue;
        }

        if (arg == "--version") {
            settings.show_version = true;
            continue;
        }

        throw std::invalid_argument("Unknown argument: " + arg);
    }

    return settings;
}

inline BridgeSettings parse_bridge_settings(int argc, char* argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));

    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    return parse_bridge_settings(args);
}

class LocalSessionManager {
private:
    class IdleShutdownMonitor {
    private:
        net::steady_timer timer_;
        std::chrono::milliseconds timeout_;
        std::function<void()> on_timeout_;
        std::size_t generation_ = 0;

        void arm_timer() {
            if (timeout_.count() <= 0 || !on_timeout_) {
                return;
            }

            const std::size_t generation = ++generation_;

            // this monitor set timeout to 0 and calles on_timeout_ immediately when generation > 1
            if (generation > 1) {
                on_timeout_();
                return;
            }

            timer_.expires_after(timeout_);
            timer_.async_wait([this, generation](const boost::system::error_code& error) {
                if (error == net::error::operation_aborted) {
                    return;
                }

                if (error || generation != generation_) {
                    return;
                }

                on_timeout_();
            });
        }

        void cancel_timer() {
            ++generation_;
            timer_.cancel();
        }

    public:
        IdleShutdownMonitor(
            net::any_io_executor executor,
            std::chrono::milliseconds timeout,
            std::function<void()> on_timeout)
            : timer_(executor),
              timeout_(timeout),
              on_timeout_(std::move(on_timeout)) {
            arm_timer();
        }

        void update(std::size_t session_count) {
            if (session_count == 0) {
                arm_timer();
                return;
            }

            cancel_timer();
        }
    };

    std::vector<std::shared_ptr<LocalSession>> sessions;
    std::unique_ptr<IdleShutdownMonitor> idle_shutdown_monitor_;

    void on_session_count_changed() {
        if (idle_shutdown_monitor_) {
            idle_shutdown_monitor_->update(sessions.size());
        }
    }

public:
    LocalSessionManager() = default;

    LocalSessionManager(
        net::any_io_executor executor,
        std::chrono::milliseconds idle_timeout,
        std::function<void()> on_idle_timeout)
        : idle_shutdown_monitor_(std::make_unique<IdleShutdownMonitor>(
            executor,
            idle_timeout,
            std::move(on_idle_timeout))) {}

    std::shared_ptr<LocalSession> register_session(std::shared_ptr<LocalSession> session) {
        sessions.push_back(std::move(session));
        on_session_count_changed();
        return sessions.back();
    }

    void unregister(std::shared_ptr<LocalSession> session) {
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                [&session](const std::shared_ptr<LocalSession>& current) {
                    return current == session;
                }),
            sessions.end()
        );
        on_session_count_changed();
    }

    std::size_t session_count() const {
        return sessions.size();
    }

    net::awaitable<void> broadcast(std::string message) {
        std::vector<std::shared_ptr<LocalSession>> sessions_copy = sessions;
        std::vector<std::shared_ptr<LocalSession>> dead_sessions;
        std::string framed_message = lsp::frame_message(message);

        for (auto& session : sessions_copy) {
            if (!session->is_open()) {
                dead_sessions.push_back(session);
                continue;
            }

            try {
                co_await session->write_framed(framed_message);
            } catch (...) {
                dead_sessions.push_back(session);
            }
        }

        if (!dead_sessions.empty()) {
            sessions.erase(
                std::remove_if(sessions.begin(), sessions.end(),
                    [&dead_sessions](const std::shared_ptr<LocalSession>& current) {
                        return std::find(dead_sessions.begin(), dead_sessions.end(), current) != dead_sessions.end();
                    }),
                sessions.end());
        }
        on_session_count_changed();
    }
};

