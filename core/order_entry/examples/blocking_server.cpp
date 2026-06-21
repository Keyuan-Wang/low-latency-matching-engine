#include "order_entry/frame_parser.hpp"

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>


using namespace llmes::order_entry;

namespace {

using Parser = FrameParser<4096>;

void set_reuseaddr(int fd) {
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

void set_tcp_nodelay(int fd) {
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}


void print_message(const DecodedMessage& msg) {
    std::cout << "seq=" << msg.header.sequence_numer
    << " session=" << msg.header.session_id
    << " type=" << static_cast<std::uint16_t>(msg.type);

    switch (msg.type) {
        case MessageType::NewOrder:
        std::cout << " NewOrder"
                    << " id=" << msg.new_order.client_order_id
                    << " side=" << static_cast<std::uint64_t>(msg.new_order.side)
                    << " price=" << msg.new_order.price
                    << " qty=" << msg.new_order.quantity;
        break;

        case MessageType::CancelOrder:
        std::cout << " CancelOrder"
                    << " id=" << msg.cancel_order.client_order_id;
        break;

        case MessageType::ModifyOrder:
        std::cout << " ModifyOrder"
                    << " id=" << msg.modify_order.client_order_id
                    << " price=" << msg.modify_order.new_price
                    << " qty=" << msg.modify_order.new_quantity;
        break;

        case MessageType::Heartbeat:
        std::cout << " Heartbeat";
        break;

        case MessageType::Logout:
        std::cout << " Logout";
        break;

        default:
        std::cout << " Unknown";
        break;
    }
    
    std::cout << '\n';
}

}   // namespace


int main() {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    set_reuseaddr(listen_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(9000);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        ::close(listen_fd);
        return 1;
    }

    if(::listen(listen_fd, 16) < 0) {
        perror("listen");
        ::close(listen_fd);
        return 1;
    }

    std::cout << "order_entry blocking server listening on 127.0.0.1:9000\n";

    const int client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) {
        perror("accept");
        ::close(listen_fd);
        return 1;
    }

    set_tcp_nodelay(client_fd);


    Parser parser;
    std::array<std::byte, 1024> read_buf{};

    while (true) {
        const ssize_t n = ::recv(client_fd, read_buf.data(), read_buf.size(), 0);

        if (n == 0) {
            std::cout << "client closed\n";
            break;
        }

        if (n < 0) {
            perror("recv");
            break;
        }

        const auto bytes = std::span<const std::byte>(
                                                    read_buf.data(), 
                                                    static_cast<std::size_t>(n));
        
        // append received data to buffer
        if (!parser.append(bytes)) {
            std::cerr << "parser input buffer full" << std::endl;
            break;
        }

        while (true) {
            DecodedMessage msg;
            const auto status = parser.try_parse(msg);

            // wait for more data
            if (status == Parser::Status::NeedMoreData) {
                break;
            }

            if (status == Parser::Status::ProtocolError) {
                std::cerr << "protocol error, closing session\n";
                ::close(client_fd);
                ::close(listen_fd);
                return 1;
            }

            print_message(msg);
            

            // check if client closed
            if (msg.type == MessageType::Logout) {
                std::cout << "logout received\n";
                ::close(client_fd);
                ::close(listen_fd);
                return 0;
            }
        }
    }

    ::close(client_fd);
    ::close(listen_fd);
    return 0;
}