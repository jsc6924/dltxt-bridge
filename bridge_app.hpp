#pragma once

#include <boost/asio.hpp>

#include <algorithm>
#include <csignal>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bridge_protocol.hpp"

namespace net = boost::asio;
using tcp = net::ip::tcp;

struct BridgeSettings {
    unsigned short local_port = 6009;
    std::string remote_host = "127.0.0.1";
    std::string remote_port = "9000";
    std::chrono::milliseconds request_timeout = std::chrono::seconds(30);
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

inline bool socket_is_dead(const std::shared_ptr<tcp::socket>& socket) {
    return !socket || !socket->is_open();
}

inline void close_socket_if_open(const std::shared_ptr<tcp::socket>& socket) {
    if (!socket || !socket->is_open()) {
        return;
    }

    boost::system::error_code error;
    socket->shutdown(tcp::socket::shutdown_both, error);
    socket->close(error);
}

inline void erase_sockets_by_identity(
    std::vector<std::shared_ptr<tcp::socket>>& sockets,
    const std::vector<std::shared_ptr<tcp::socket>>& dead_sockets) {

    if (dead_sockets.empty()) {
        return;
    }

    sockets.erase(
        std::remove_if(sockets.begin(), sockets.end(),
            [&dead_sockets](const std::shared_ptr<tcp::socket>& socket) {
                return std::any_of(dead_sockets.begin(), dead_sockets.end(),
                    [&socket](const std::shared_ptr<tcp::socket>& dead_socket) {
                        return dead_socket.get() == socket.get();
                    });
            }),
        sockets.end()
    );
}

inline unsigned short parse_port_number(const std::string& value, const char* option_name) {
    try {
        const auto parsed = std::stoul(value);
        if (parsed == 0 || parsed > 65535) {
            throw std::out_of_range("port out of range");
        }

        return static_cast<unsigned short>(parsed);
    } catch (...) {
        throw std::invalid_argument(std::string("Invalid value for ") + option_name + ": " + value);
    }
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
        if (arg == "--listen-port") {
            if (index + 1 >= args.size()) {
                throw std::invalid_argument("Missing value for --listen-port");
            }

            settings.local_port = parse_port_number(args[++index], "--listen-port");
            continue;
        }

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
    std::vector<std::shared_ptr<tcp::socket>> sockets_;

public:
    void register_socket(std::shared_ptr<tcp::socket> socket) {
        sockets_.push_back(std::move(socket));
    }

    void unregister_socket(const std::shared_ptr<tcp::socket>& socket) {
        sockets_.erase(
            std::remove_if(sockets_.begin(), sockets_.end(),
                [&socket](const std::shared_ptr<tcp::socket>& current) {
                    return current.get() == socket.get();
                }),
            sockets_.end()
        );
    }

    std::size_t socket_count() const {
        return sockets_.size();
    }

    net::awaitable<void> broadcast(std::string message) {
        std::vector<std::shared_ptr<tcp::socket>> sockets_copy = sockets_;
        std::vector<std::shared_ptr<tcp::socket>> dead_sockets;
        std::string framed_message = lsp::frame_message(message);

        for (auto& socket : sockets_copy) {
            if (socket_is_dead(socket)) {
                dead_sockets.push_back(socket);
                continue;
            }

            try {
                co_await net::async_write(*socket, net::buffer(framed_message), net::use_awaitable);
            } catch (...) {
                dead_sockets.push_back(socket);
            }
        }

        erase_sockets_by_identity(sockets_, dead_sockets);
    }
};