#pragma once
#include <memory>
#include <set>
#include <string>
#include <boost/asio.hpp>

namespace net = boost::asio;
using tcp = net::ip::tcp;

class LocalSession {
    std::shared_ptr<tcp::socket> socket_;
    std::set<std::string> subscribed_projects_;
public:
    explicit LocalSession(std::shared_ptr<tcp::socket> socket)
        : socket_(std::move(socket)) {}

    explicit LocalSession(tcp::socket socket)
        : LocalSession(std::make_shared<tcp::socket>(std::move(socket))) {}

    std::shared_ptr<tcp::socket> get_socket() const {
        return socket_;
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
