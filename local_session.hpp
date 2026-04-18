#pragma once
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <boost/asio.hpp>

namespace net = boost::asio;
using tcp = net::ip::tcp;

class LocalSession {
    std::shared_ptr<tcp::socket> socket_;
    net::strand<net::any_io_executor> strand_;
    std::deque<std::string> write_queue_;
    bool write_in_progress_ = false;
    std::set<std::string> subscribed_projects_;
public:
    explicit LocalSession(std::shared_ptr<tcp::socket> socket)
        : socket_(std::move(socket)),
          strand_(net::make_strand(socket_->get_executor())) {}

    explicit LocalSession(tcp::socket socket)
        : LocalSession(std::make_shared<tcp::socket>(std::move(socket))) {}

    std::shared_ptr<tcp::socket> get_socket() const {
        return socket_;
    }

    bool is_open() const {
        return socket_ && socket_->is_open();
    }

    net::awaitable<void> write_framed(std::string framed_message) {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (!is_open()) {
            throw boost::system::system_error(net::error::operation_aborted);
        }

        write_queue_.push_back(std::move(framed_message));
        if (write_in_progress_) {
            co_return;
        }

        write_in_progress_ = true;

        try {
            while (!write_queue_.empty()) {
                std::string next = std::move(write_queue_.front());
                write_queue_.pop_front();
                co_await net::async_write(*socket_, net::buffer(next), net::use_awaitable);
            }
        } catch (...) {
            write_in_progress_ = false;
            throw;
        }

        write_in_progress_ = false;
    }

    void register_subscription(const std::string& project_id) {
        subscribed_projects_.insert(project_id);
    }

    bool is_subscribed_to(const std::string& project_id) const {
        return subscribed_projects_.find(project_id) != subscribed_projects_.end();
    }

    void unregister_subscription(const std::string& project_id) {
        subscribed_projects_.erase(project_id);
    }

    bool operator==(const LocalSession& other) const {
        return socket_.get() == other.socket_.get();
    }
};

inline void erase_sockets_by_identity(
    std::vector<std::shared_ptr<LocalSession>>& sessions,
    const std::vector<std::shared_ptr<tcp::socket>>& dead_sockets) {

    if (dead_sockets.empty()) {
        return;
    }

    sessions.erase(
        std::remove_if(sessions.begin(), sessions.end(),
            [&dead_sockets](const std::shared_ptr<LocalSession>& session) {
                return std::any_of(dead_sockets.begin(), dead_sockets.end(),
                    [&session](const std::shared_ptr<tcp::socket>& dead_socket) {
                        return dead_socket.get() == session->get_socket().get();
                    });
            }),
        sessions.end()
    );
}
