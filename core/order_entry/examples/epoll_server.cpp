#include "order_entry/frame_parser.hpp"
#include "order_entry/session.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
#include <cstdio>
#include <iostream>
#include <utility>

using namespace llmes::order_entry;

namespace {

constexpr int kMaxEvents    = 64;
constexpr int kPort         = 9000;

using Parser = FrameParser<4096>;
using Frame = std::array<std::byte, kFrameSize>;

struct Connection {
    Parser parser;
    OrderEntrySession session;
    int fd;

    explicit Connection(int fd, std::uint64_t session_id)
        : session(session_id), fd(fd) {}
};

void set_reuseaddr(int fd) {
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_tcp_nodelay(int fd) {
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}


int create_listen_socket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    set_reuseaddr(fd);
    set_nonblocking(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(kPort);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 128) < 0) {
        perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}


void epoll_add(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        perror("epoll_ctl ADD");
}

void epoll_remove(int epoll_fd, int fd) {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

void close_connection(int epoll_fd, std::unordered_map<int, Connection>& connections, int fd) {
    epoll_remove(epoll_fd, fd);
    connections.erase(fd);
    ::close(fd);
}

}   // namespace

int main () {

    // ======================== 1. Listen socket ========================

    const int listen_fd = create_listen_socket();
    if (listen_fd < 0)
        return 1;

    std::cout << "epoll server listening on 127.0.0.1:9000" << std::endl;


    // ======================== 2. Epoll instance ========================

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        ::close(listen_fd);
        return 1;
    }

    epoll_add(epoll_fd, listen_fd, EPOLLIN);


    // ======================== 3. Per-connection state ========================

    std::unordered_map<int, Connection> connections;
    std::uint64_t next_session_id = 1;


    // ======================== 4. Event loop ========================

    std::array<epoll_event, kMaxEvents> events{};

    while(true) {
        const int n = ::epoll_wait(epoll_fd, events.data(), kMaxEvents, -1);

        if (n < 0) {
            if (errno == EINTR) 
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;

            // ---------- 4a. New connection ----------

            if (fd == listen_fd) {
                while(true) {
                    const int client_fd = ::accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept");
                        break;
                    }

                    set_nonblocking(client_fd);
                    set_tcp_nodelay(client_fd);

                    epoll_add(epoll_fd, client_fd, EPOLLIN);

                    const auto sid = next_session_id++;
                    connections.emplace(std::piecewise_construct, 
                                        std::forward_as_tuple(client_fd), 
                                        std::forward_as_tuple(client_fd, sid));

                    std::cout << "accpeted fd = " << client_fd << " session = " << sid << std::endl;
                }

                continue;
            }

            // ---------- 4b. Client data ready ----------

            if (!(events[i].events & EPOLLIN))
                continue;
            
            auto it = connections.find(fd);
            if (it == connections.end())
                continue;

            auto& conn = it->second;

            std::array<std::byte, 1024> read_buf{};
            const ssize_t nbytes = ::recv(fd, read_buf.data(), read_buf.size(), 0);

            if (nbytes == 0) {
                std::cout << "fd = " << fd << " client cloased" << std::endl;
                close_connection(epoll_fd, connections, fd);
                continue;
            }

            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                perror("recv");
                close_connection(epoll_fd, connections, fd);
                continue;
            }

            const auto bytes = std::span<const std::byte>(
                read_buf.data(), static_cast<std::size_t>(nbytes));

            if (!conn.parser.append(bytes)) {
                std::cerr << "fd=" << fd << " parser buffer full\n";
                close_connection(epoll_fd, connections, fd);
                continue;
            }

            bool should_close = false;

            while (true) {
                DecodedMessage msg;
                const auto status = conn.parser.try_parse(msg);

                if (status == Parser::Status::NeedMoreData)
                    break;

                if (status == Parser::Status::ProtocolError) {
                    std::cerr << "fd=" << fd << " protocol error\n";
                    should_close = true;
                    break;
                }

                Frame response{};
                const bool keep_open =
                    conn.session.handle_message(msg, response);

                const ssize_t sent = ::send(fd, response.data(),
                                            response.size(), MSG_NOSIGNAL);
                if (sent != static_cast<ssize_t>(response.size())) {
                    std::cerr << "fd=" << fd << " send incomplete\n";
                    should_close = true;
                    break;
                }

                if (!keep_open) {
                    std::cout << "fd=" << fd << " logout\n";
                    should_close = true;
                    break;
                }
            }

            if (should_close)
                close_connection(epoll_fd, connections, fd);
        }
    }

    for (auto& [fd, _] : connections)
        ::close(fd);

    ::close(epoll_fd);
    ::close(listen_fd);
    return 0;
}
