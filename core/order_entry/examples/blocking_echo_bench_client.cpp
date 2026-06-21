#include "order_entry/codec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace llmes::order_entry;

namespace {

using Frame = std::array<std::byte, kFrameSize>;

struct Options {
    const char* host = "127.0.0.1";
    int port = 9000;
    std::uint64_t iterations = 100000;
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            opts.host = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--port") == 0) {
            opts.port = std::atoi(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--iterations") == 0) {
            opts.iterations = static_cast<std::uint64_t>(std::strtoull(argv[i + 1], nullptr, 10));
            ++i;
        }
    }
    return opts;
}

void set_tcp_nodelay(int fd) {
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

Frame make_new_order() {
    Frame frame{};

    MessageHeader header;
    header.sequence_numer = 1;
    header.session_id = 42;

    NewOrder order;
    order.client_order_id = 1001;
    order.side = Side::Buy;
    order.price = 12345;
    order.quantity = 10;

    const auto n = encode_new_order(header, order, frame);
    (void)n;

    return frame;
}

} // namespace

int main(int argc, char** argv) {
    const Options opts = parse_options(argc, argv);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    set_tcp_nodelay(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(opts.port));

    ::inet_pton(AF_INET, opts.host, &addr.sin_addr);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    const Frame request = make_new_order();
    Frame reply{};

    const auto start = std::chrono::steady_clock::now();

    for (std::uint64_t i = 0; i < opts.iterations; ++i) {
        ::send(fd, request.data(), request.size(), 0);
        ::recv(fd, reply.data(), reply.size(), MSG_WAITALL);
    }

    const auto end = std::chrono::steady_clock::now();
    const auto total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double avg_rtt_ns = static_cast<double>(total_ns) / static_cast<double>(opts.iterations);

    std::cout << "messages=" << opts.iterations << '\n';
    std::cout << "total_ns=" << total_ns << '\n';
    std::cout << "avg_rtt_ns=" << avg_rtt_ns << '\n';
    std::cout << "avg_one_way_est_ns=" << avg_rtt_ns / 2.0 << '\n';

    ::close(fd);
    return 0;
}
