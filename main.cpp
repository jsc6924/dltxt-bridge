#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// A simple session class to handle one local VS Code window connection
class LocalSession : public std::enable_shared_from_this<LocalSession> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;

public:
    explicit LocalSession(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() {
        do_read();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(net::buffer(buffer_.prepare(1024)),
            [this, self](beast::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    buffer_.commit(bytes_transferred);
                    
                    // Convert buffer to string/json
                    std::string msg{beast::buffers_to_string(buffer_.data())};
                    std::cout << "[Local] Received: " << msg << std::endl;
                    
                    // Logic: Forward 'msg' to the remote WebSocket here...

                    buffer_.consume(bytes_transferred);
                    do_read();
                }
            });
    }
};

int main() {
    try {
        net::io_context ioc;
        const auto address = net::ip::make_address("127.0.0.1");
        const unsigned short port = 6009;

        // The Singleton Check: Try to bind the port
        tcp::acceptor acceptor{ioc, {address, port}};
        std::cout << "dltxt_bridge started on port " << port << std::endl;

        // In a real app, you'd start the Remote WebSocket client here
        // websocket::stream<tcp::socket> ws_remote{ioc}; 

        // Accept local connections from VS Code windows
        std::function<void()> do_accept = [&]() {
            acceptor.async_accept([&](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<LocalSession>(std::move(socket))->start();
                }
                do_accept();
            });
        };

        do_accept();
        ioc.run(); // Blocks and handles all async events

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "Is another instance of dltxt_bridge already running?" << std::endl;
        return 1;
    }
    return 0;
}