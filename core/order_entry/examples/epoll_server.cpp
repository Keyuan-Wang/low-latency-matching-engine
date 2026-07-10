#include "order_entry/codec.hpp"
#include "order_entry/engine_messages.hpp"
#include "order_entry/frame_parser.hpp"
#include "order_entry/protocol.hpp"
#include "spsc_ring_buffer.hpp"

#include <arpa/inet.h>
#include <cerrno>
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

    auto close_conn = [&](int fd) {
        auto it = connections.find(fd);
        if(it != connections.end()) {
            auto& tok = it->second.token;
            slots[tok.slot].fd = -1;

            // session close
            EngineCommand cmd{};
            cmd.type = EngineCommandType::SessionClosed;
            cmd.session = tok;
            cmd_queue.push(cmd);

            connections.erase(it);
        }
        epoll_remove(epoll_fd, fd);
        ::close(fd);
    };

    // keep listening
    while(true) {
        const int n = ::epoll_wait(epoll_fd, events.data(), kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;

            // ---------- 5a. eventfd: response from matching engine ----------

            if (fd == event_fd) {
                std::uint64_t val;
                ::read(event_fd, &val, sizeof(val));    // consume eventfd

                EngineResponse rsp;
                while (rsp_queue.pop(rsp)) {
                    auto& slot = slots[rsp.session.slot];
                    if (slot.generation != rsp.session.generation || slot.fd < 0)
                        continue;   // session

                    Frame frame{};
                    encode_response(rsp, frame);

                    const ssize_t sent = ::send(slot.fd, frame.data(), frame.size(), MSG_NOSIGNAL);

                    if (sent != static_cast<ssize_t>(frame.size())) {
                        std::cerr << "fd=" << slot.fd << " send failed\n";
                        close_conn(slot.fd);
                    }
                }
                continue;
            }

            // ---------- 5b. New connection ----------

            if (fd == listen_fd) {
                while(true) {
                    const int client_fd = ::accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)    break;
                        perror("accept");
                        break;
                    }

                    set_nonblocking(client_fd);
                    set_tcp_nodelay(client_fd);
                    epoll_add(epoll_fd, client_fd, EPOLLIN);

                    // allocate SessionToken
                    const std::uint32_t s = next_slot++ % kMaxSlots;
                    slots[s].generation++;
                    slots[s].fd = client_fd;
                    SessionToken tok{s, slots[s].generation};

                    connections.emplace(
                        std::piecewise_construct,
                        std::forward_as_tuple(client_fd),
                        std::forward_as_tuple(client_fd, tok));

                    std::cout << "accepted fd=" << client_fd << " slot=" << s << '\n';
                }
            }

            // ---------- 5c. Client data → parse → push command ----------

            if (!(events[i].events & EPOLLIN))  continue;

            auto it = connections.find(fd);
            if (it == connections.end()) continue;
            auto& conn = it->second;

            std::array<std::byte, 1024> read_buf{};
            const ssize_t nbytes = ::recv(fd, read_buf.data(), read_buf.size(), 0);

            if (nbytes == 0) {
                std::cout << "fd=" << fd << " closed\n";
                close_conn(fd); continue;
            }
            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                perror("recv"); close_conn(fd); continue;
            }

            if (!conn.parser.append({read_buf.data(),
                                     static_cast<std::size_t>(nbytes)})) {
                std::cerr << "fd=" << fd << " buffer full\n";
                close_conn(fd); continue;
            }

            bool should_close = false;

            while (true) {
                DecodedMessage msg;
                const auto status = conn.parser.try_parse(msg);

                if (status == Parser::Status::NeedMoreData)     break;
                if (status == Parser::Status::ProtocolError) {
                    should_close = true;
                    break;
                }

                // fast rejection pathes
                if (msg.header.sequence_number != conn.expected_sequence) {
                    Frame frame{};
                    MessageHeader rh = msg.header;
                    rh.session_id = conn.token.slot;
                    Rejected rej{};
                    rej.reason = RejectReason::BadSequence;
                    encode_rejected(rh, rej, frame);
                    ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                    continue;
                }

                ++conn.expected_sequence;

                if (msg.type == MessageType::Logout) {
                    Frame frame{};
                    MessageHeader rh = msg.header;
                    Accepted acc{};
                    encode_accepted(rh, acc, frame);
                    ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                    should_close = true;
                    break;
                }

                if (msg.type == MessageType::Heartbeat) {
                    Frame frame{};
                    MessageHeader rh = msg.header;
                    Accepted acc{};
                    encode_accepted(rh, acc, frame);
                    ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                    continue;
                }


                // Real order msg: convert to enginecommand, push to matching engine
                EngineCommand cmd{};
                cmd.session = conn.token;
                cmd.request_sequence = msg.header.sequence_number;

                switch (msg.type) {
                    case MessageType::NewOrder:
                        cmd.type = EngineCommandType::NewLimit;
                        cmd.client_order_id = msg.new_order.client_order_id;
                        cmd.side = msg.new_order.side;
                        cmd.price = msg.new_order.price;
                        cmd.quantity = msg.new_order.quantity;
                        break;
                    case MessageType::CancelOrder:
                        cmd.type = EngineCommandType::Cancel;
                        cmd.client_order_id = msg.cancel_order.client_order_id;
                        break;
                    case MessageType::ModifyOrder:
                        cmd.type = EngineCommandType::Modify;
                        cmd.client_order_id = msg.modify_order.client_order_id;
                        cmd.price = msg.modify_order.new_price;
                        cmd.quantity = msg.modify_order.new_quantity;
                        break;
                    default:
                        break;
                }

                if (!cmd_queue.push(cmd)) {
                    std::cerr << "command queue full!\n";
                }
                
                if (should_close) close_conn(fd);
            }
        }
    }

    EngineCommand shutdown{};
    shutdown.type = EngineCommandType::Shutdown;
    cmd_queue.push(shutdown);
    matching.join();

    for (auto& [fd, _] : connections) ::close(fd);
    ::close(epoll_fd);
    ::close(event_fd);
    ::close(listen_fd);
    return 0;
}
