#include "order_entry/codec.hpp"
#include "order_entry/engine_messages.hpp"
#include "order_entry/frame_parser.hpp"
#include "order_entry/protocol.hpp"
#include "spsc_ring_buffer.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>


#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <unordered_map>



using namespace llmes::order_entry;
using namespace llmes::spsc;

namespace {

constexpr int kMaxEvents                = 64;
constexpr int kPort                     = 9000;
constexpr std::size_t kQueueCapacity    = 4096;

using Parser = FrameParser<4096>;
using Frame = std::array<std::byte, kFrameSize>;
using CommandQueue  = SpscRingBufferAtomicV3<EngineCommand, kQueueCapacity>;
using ResponseQueue = SpscRingBufferAtomicV3<EngineResponse, kQueueCapacity>;


// Connection onlds SessionToken to route back to correct fd

struct Connection {
    Parser parser;
    SessionToken token;
    std::uint64_t expected_sequence = 1;
    int fd;

    explicit Connection(int fd, SessionToken tok)
        : token(tok), fd(fd) {}
};

// mapping from sessiontoken.slot to fd,
// engine finds correct socket according to this
struct SessionSlot {
    int fd = -1;
    std::uint32_t generation = 0;
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


// =====================================================================
// Matching thread — spin-poll command queue, push response, write eventfd
// =====================================================================

void matching_thread(CommandQueue& cmd_q, ResponseQueue& rsp_q,
                     int notify_fd, std::atomic<bool>& running) {
    while (running.load(std::memory_order_relaxed)) {
        EngineCommand cmd;
        if (!cmd_q.pop(cmd))
            continue;

        if (cmd.type == EngineCommandType::Shutdown)
            break;
        
        // Receive response
        EngineResponse rsp{};
        rsp.session = cmd.session;
        rsp.client_order_id = cmd.client_order_id;

        switch (cmd.type) {
            case EngineCommandType::NewLimit:
                rsp.type = EngineResponseType::Accepted;
                break;
            case EngineCommandType::Cancel:
                rsp.type = EngineResponseType::Cancelled;
                break;
            case EngineCommandType::Modify:
                rsp.type = EngineResponseType::Modified;
                break;
            case EngineCommandType::SessionClosed:
                continue;
            default:
                continue;
        }

        rsp_q.push(rsp);

        // Notify the gateway there is response to consume
        const std::uint64_t one = 1;
        ::write(notify_fd, &one, sizeof(one));
    }
}

// =====================================================================
// Gateway thread — decode and push command；drain response when eventfd is readable
// =====================================================================

// Decode EngineResponse to 64 bytes then write back to client
void encode_response(const EngineResponse& rsp, std::span<std::byte> out) {
    MessageHeader h{};
    h.payload_length = kPayloadSize;

    switch (rsp.type) {
        case EngineResponseType::Accepted: {
            Accepted a{};
            a.client_order_id = rsp.client_order_id;
            encode_accepted(h, a, out);
            break;
        }
        case EngineResponseType::Rejected: {
            Rejected r{};
            r.client_order_id = rsp.client_order_id;
            r.reason = rsp.reject_reason;
            encode_rejected(h, r, out);
            break;
        }
        case EngineResponseType::Cancelled: {
            Cancelled c{};
            c.client_order_id = rsp.client_order_id;
            encode_cancelled(h, c, out);
            break;
        }
        case EngineResponseType::Modified: {
            Modified m{};
            m.client_order_id = rsp.client_order_id;
            encode_modified(h, m, out);
            break;
        }
        case EngineResponseType::Trade: {
            Trade t{};
            t.client_order_id = rsp.client_order_id;
            t.price = rsp.price;
            t.quantity = rsp.quantity;
            encode_trade(h, t, out);
            break;
        }
    }
}

}   // namespace

int main () {

    // ======================== 1. Listen socket ========================

    const int listen_fd = create_listen_socket();
    if (listen_fd < 0)
        return 1;

    std::cout << "epoll server listening on 127.0.0.1:9000" << std::endl;


    // ======================== 2. SPSC queues + eventfd ========================

    CommandQueue cmd_queue;
    ResponseQueue rsp_queue;
    std::atomic<bool> engine_running{true};

    // eventfd: matching thread writes to it to wake epoll_wait
    const int event_fd = ::eventfd(0, EFD_NONBLOCK);
    if (event_fd < 0) {
        perror("eventfd");
        ::close(listen_fd);
        return 1;
    }

    std::thread matching(matching_thread,
                         std::ref(cmd_queue), std::ref(rsp_queue),
                         event_fd, std::ref(engine_running));


    // ======================== 3. Epoll instance ========================

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        ::close(listen_fd);
        return 1;
    }

    epoll_add(epoll_fd, listen_fd, EPOLLIN);
    epoll_add(epoll_fd, event_fd, EPOLLIN);

    // ======================== 4. Per-connection state ========================

    std::unordered_map<int, Connection> connections;
    
    // slot allocator, simply increase
    // generation to prevent ABA
    constexpr std::uint32_t kMaxSlots = 1024;
    std::array<SessionSlot, kMaxSlots> slots{};
    std::uint32_t next_slot = 0;


    // ======================== 5. Event loop ========================

    std::array<epoll_event, kMaxEvents> events{};

    
}
