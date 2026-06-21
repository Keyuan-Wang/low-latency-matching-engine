#pragma once

#include <cstdint>
#include <cstddef>


namespace llmes::order_entry {

inline constexpr std::uint32_t kMagic = 0x6c6c6d65; // "llme"

inline constexpr std::uint16_t kVersion = 1;

inline constexpr std::size_t kFrameSize = 64;
inline constexpr std::size_t kHeaderSize = 32;
inline constexpr std::size_t kPayloadSize = 32;


// compact message
enum class MessageType : std::uint16_t {
    NewOrder        = 1,
    CancelOrder     = 2,
    ModifyOrder     = 3,
    Heartbeat       = 4,
    Logout          = 5,

    Accepted        = 101,
    Rejected        = 102,
    Cancelled       = 103,
    Modified        = 104,
    Trade           = 105
};


enum class Side : std::uint64_t {
    Buy  = 1,
    Sell = 2,
};


enum class DecodeStatus {
    Ok,
    NeedMoreData,
    BadMagic,
    BadVersion,
    UnknownMessageType,
    BadPayloadLength
};


// 32-bytes
struct MessageHeader {
    std::uint32_t magic                             = kMagic;
    std::uint16_t version                           = kVersion;
    MessageType message_type{};
    std::uint16_t payload_length                    = 0;
    std::uint16_t flags                             = 0;
    std::uint64_t sequence_numer                    = 0;
    std::uint64_t session_id                        = 0;
    std::uint32_t reserved                          = 0;

    static constexpr std::size_t off_magic          = 0;
    static constexpr std::size_t off_version        = 4;
    static constexpr std::size_t off_message_type   = 6;
    static constexpr std::size_t off_payload_length = 8;
    static constexpr std::size_t off_flags          = 10;
    static constexpr std::size_t off_sequence_numer = 12;
    static constexpr std::size_t off_session_id     = 20;
    static constexpr std::size_t off_reserved       = 28;
};


// 32-bytes
struct NewOrder {
    std::uint64_t client_order_id               = 0;
    Side side                                   = Side::Buy;
    std::uint64_t price                         = 0;
    std::uint64_t quantity                      = 0;

    static constexpr std::size_t off_id         = 0;
    static constexpr std::size_t off_side       = 8;
    static constexpr std::size_t off_price      = 16;
    static constexpr std::size_t off_quantity   = 24;
};


// 32-bytes
struct CancelOrder {
    std::uint64_t client_order_id               = 0;
    std::uint64_t reserved1;
    std::uint64_t reserved2;
    std::uint64_t reserved3;

    static constexpr std::size_t off_id         = 0;
};


// 32-bytes
struct ModifyOrder {
    std::uint64_t client_order_id                   = 0;
    std::uint64_t new_price                         = 0;
    std::uint64_t new_quantity                      = 0;
    std::uint64_t reserved;

    static constexpr std::size_t off_id             = 0;
    static constexpr std::size_t off_new_price      = 8;
    static constexpr std::size_t off_new_quantity   = 16;
};


enum class RejectReason : std::uint64_t {
    None = 0,
    BadSequence = 1,
    DuplicateClientOrderId = 2,
    UnknownClientOrderId = 3,
    InvalidPrice = 4,
    InvalidQuantity = 5,
    UnsupportedMessageType = 6,
};

struct Accepted {
    std::uint64_t client_order_id = 0;
    std::uint64_t order_handle = 0;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;

    static constexpr std::size_t off_client_order_id = 0;
    static constexpr std::size_t off_order_handle = 8;
    static constexpr std::size_t off_reserved1 = 16;
    static constexpr std::size_t off_reserved2 = 24;
};

struct Rejected {
    std::uint64_t client_order_id = 0;
    RejectReason reason = RejectReason::None;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;

    static constexpr std::size_t off_client_order_id = 0;
    static constexpr std::size_t off_reason = 8;
    static constexpr std::size_t off_reserved1 = 16;
    static constexpr std::size_t off_reserved2 = 24;
};

struct Cancelled {
    std::uint64_t client_order_id = 0;
    std::uint64_t order_handle = 0;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;

    static constexpr std::size_t off_client_order_id = 0;
    static constexpr std::size_t off_order_handle = 8;
    static constexpr std::size_t off_reserved1 = 16;
    static constexpr std::size_t off_reserved2 = 24;
};

struct Modified {
    std::uint64_t client_order_id = 0;
    std::uint64_t order_handle = 0;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;

    static constexpr std::size_t off_client_order_id = 0;
    static constexpr std::size_t off_order_handle = 8;
    static constexpr std::size_t off_reserved1 = 16;
    static constexpr std::size_t off_reserved2 = 24;
};

struct Trade {
    std::uint64_t client_order_id = 0;
    std::uint64_t order_handle = 0;
    std::uint64_t price = 0;
    std::uint64_t quantity = 0;

    static constexpr std::size_t off_client_order_id = 0;
    static constexpr std::size_t off_order_handle = 8;
    static constexpr std::size_t off_price = 16;
    static constexpr std::size_t off_quantity = 24;
};


// 32-bytes
struct Hearbeat {
    std::uint64_t reserved1;
    std::uint64_t reserved2;
    std::uint64_t reserved3;
    std::uint64_t reserved4;
};


// 32-bytes
struct Logout {
    std::uint64_t reserved1;
    std::uint64_t reserved2;
    std::uint64_t reserved3;
    std::uint64_t reserved4;
};

}   // namespace llmes::order_entry
