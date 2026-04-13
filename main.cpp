#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <vector>
#include <mutex>

namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;
using json = nlohmann::json;
namespace websocket = beast::websocket;

// Manager for active local VS Code sessions
class LocalSessionManager {
private:
    std::vector<std::shared_ptr<tcp::socket>> sockets_;
    std::mutex mutex_;

public:
    void register_socket(std::shared_ptr<tcp::socket> socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_.push_back(socket);
    }

    void unregister_socket(std::shared_ptr<tcp::socket> socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_.erase(
            std::remove_if(sockets_.begin(), sockets_.end(),
                [socket](const std::shared_ptr<tcp::socket>& s) {
                    return s.get() == socket.get();
                }),
            sockets_.end()
        );
    }

    net::awaitable<void> broadcast(std::string message) {
        std::vector<std::shared_ptr<tcp::socket>> sockets_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sockets_copy = sockets_;
        }
        
        for (auto& socket : sockets_copy) {
            if (socket && socket->is_open()) {
                try {
                    co_await socket->async_write_some(
                        net::buffer(message),
                        net::use_awaitable
                    );
                } catch (...) {
                    // Socket may be closed, skip
                }
            }
        }
    }
};

class RemoteClient {
    websocket::stream<beast::tcp_stream> ws_;

public:
    explicit RemoteClient(net::any_io_executor ex) : ws_(ex) {}

    net::awaitable<void> connect(std::string host, std::string port) {
        tcp::resolver resolver(ws_.get_executor());
        auto const results = co_await resolver.async_resolve(host, port, net::use_awaitable);

        // Connect and Handshake
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        co_await beast::get_lowest_layer(ws_).async_connect(results, net::use_awaitable);
        
        // Turn off timeout on the tcp_stream because websocket has its own
        beast::get_lowest_layer(ws_).expires_never();

        co_await ws_.async_handshake(host, "/ws", net::use_awaitable);
        std::cout << "Connected to Go Backend at " << host << ":" << port << std::endl;
    }

    net::awaitable<void> send(std::string message) {
        std::cout << "Sending to Go backend: " << message << std::endl;
        co_await ws_.async_write(net::buffer(message), net::use_awaitable);
        std::cout << "Message sent to Go backend" << std::endl;
    }

    net::awaitable<std::string> receive() {
        beast::flat_buffer buffer;
        co_await ws_.async_read(buffer, net::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    }
};

net::awaitable<void> handle_local_session(
    tcp::socket socket, 
    std::shared_ptr<RemoteClient> remote,
    std::shared_ptr<LocalSessionManager> session_manager) {
    
    auto socket_ptr = std::make_shared<tcp::socket>(std::move(socket));
    session_manager->register_socket(socket_ptr);
    
    beast::flat_buffer buffer;
    try {
        for (;;) {
            std::size_t n = co_await socket_ptr->async_read_some(buffer.prepare(1024), net::use_awaitable);
            buffer.commit(n);
            
            std::string msg = beast::buffers_to_string(buffer.data());
            
            // Forward the VS Code message to the Go Backend
            co_await remote->send(msg);

            buffer.consume(n);
        }
    } catch (...) { 
        /* Handle disconnect */ 
        session_manager->unregister_socket(socket_ptr);
    }
}

// Remote reader coroutine: continuously receives from Go backend and broadcasts to all local sessions
net::awaitable<void> remote_reader(
    std::shared_ptr<RemoteClient> remote,
    std::shared_ptr<LocalSessionManager> session_manager) {
    
    try {
        for (;;) {
            std::string message = co_await remote->receive();
            std::cout << "Received from Go backend, broadcasting to local sessions" << std::endl;
            co_await session_manager->broadcast(message);
        }
    } catch (std::exception& e) {
        std::cerr << "Remote reader error: " << e.what() << std::endl;
    }
}

// Coroutine to listen for new VS Code windows
net::awaitable<void> listener(
    unsigned short port, 
    std::shared_ptr<RemoteClient> remote_client_ptr,
    std::shared_ptr<LocalSessionManager> session_manager) {
    auto executor = co_await net::this_coro::executor;
    tcp::acceptor acceptor(executor, {tcp::v4(), port});
    
    std::cout << "dltxt_bridge (Coroutine mode) on port " << port << std::endl;

    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);

        // Pass the coroutine as a lambda or ensure the return type 'net::awaitable<void>' is visible
        net::co_spawn(socket.get_executor(), 
            handle_local_session(std::move(socket), remote_client_ptr, session_manager), 
            net::detached);
    }
}

int main() {
    try {
        net::io_context ioc;

        auto remote_client_ptr = std::make_shared<RemoteClient>(ioc.get_executor());
        auto session_manager = std::make_shared<LocalSessionManager>();

        // Launch ONE coordinator coroutine to handle startup order
        net::co_spawn(ioc, [remote_client_ptr, session_manager]() mutable -> net::awaitable<void> {
            auto ex = co_await net::this_coro::executor;
            net::co_spawn(ex, listener(6009, remote_client_ptr, session_manager), net::detached);

            for(;;) {
                try {
                    // WAIT for the connection to be fully established
                    co_await remote_client_ptr->connect("127.0.0.1", "9000");

                    // Start the loops that depend on that connection
                    co_await remote_reader(remote_client_ptr, session_manager);
                } catch (std::exception& e) {
                    std::cerr << "Startup or Connection Error: " << e.what() << std::endl;
                }
            }
            
            co_return;
        }, net::detached);

        ioc.run();
    } catch (std::exception& e) {
        std::cerr << "Bridge Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}