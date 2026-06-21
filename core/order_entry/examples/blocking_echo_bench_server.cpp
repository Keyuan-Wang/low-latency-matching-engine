#include "order_entry/frame_parser.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace llmes::order_entry;

namespace {

using Parser = FrameParser<4096>;

int parse_port(int argc, char** argv) {
    int port = 9000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            port = std::atoi(argv[i + 1]);
            ++i;
        }
    }
    return port;
}

void set_reuseaddr(int fd) {
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

void set_tcp_nodelay(int fd) {
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

} // namespace

int main(int argc, char** argv) {
    const int port = parse_port(argc, argv);

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);

    set_reuseaddr(listen_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listen_fd, 16);

    std::cout << "order_entry echo bench server listening on 127.0.0.1:" << port << '\n';

    const int client_fd = ::accept(listen_fd, nullptr, nullptr);

    set_tcp_nodelay(client_fd);

    Parser parser;
    std::array<std::byte, kFrameSize> frame{};
    std::uint64_t parsed_messages = 0;

    while (::recv(client_fd, frame.data(), frame.size(), MSG_WAITALL) == static_cast<ssize_t>(frame.size())) {
        parser.append(frame);

        DecodedMessage msg;
        parser.try_parse(msg);

        ++parsed_messages;

        ::send(client_fd, frame.data(), frame.size(), 0);
    }

    std::cout << "parsed_messages=" << parsed_messages << '\n';

    ::close(client_fd);
    ::close(listen_fd);
    return 0;
}
