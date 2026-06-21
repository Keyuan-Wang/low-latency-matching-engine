#pragma once

#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <span>


// force little endian align

namespace llmes::order_entry {

[[gnu::always_inline]] inline void store_u16_le(std::span<std::byte> out, std::size_t offset, 
                                                std::same_as<std::uint16_t> auto value) {
    out[offset + 0] = static_cast<std::byte>(value & 0xff);             // get the low 8 digits of value
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);      // get the high 8 digits of value
}


[[gnu::always_inline]] inline void store_u32_le(std::span<std::byte> out, std::size_t offset, 
                                                std::same_as<std::uint32_t> auto value) {
    out[offset + 0] = static_cast<std::byte>(value & 0xff);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
    out[offset + 2] = static_cast<std::byte>((value >> 16) & 0xff);
    out[offset + 3] = static_cast<std::byte>((value >> 24) & 0xff);
}


[[gnu::always_inline]] inline void store_u64_le(std::span<std::byte> out, std::size_t offset, 
                                                std::same_as<std::uint64_t> auto value) {
    // loop unrolling
    out[offset + 0] = static_cast<std::byte>(value & 0xff);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
    out[offset + 2] = static_cast<std::byte>((value >> 16) & 0xff);
    out[offset + 3] = static_cast<std::byte>((value >> 24) & 0xff);
    out[offset + 4] = static_cast<std::byte>((value >> 32) & 0xff);
    out[offset + 5] = static_cast<std::byte>((value >> 40) & 0xff);
    out[offset + 6] = static_cast<std::byte>((value >> 48) & 0xff);
    out[offset + 7] = static_cast<std::byte>((value >> 56) & 0xff);
}


[[gnu::always_inline]] inline std::uint16_t load_u16_le(std::span<const std::byte> in, std::size_t offset) {

    std::uint16_t res = 0;
    res |= static_cast<std::uint16_t>(in[offset + 0]);
    res |= static_cast<std::uint16_t>(in[offset + 1]) << 8;

    return res;
}


[[gnu::always_inline]] inline std::uint32_t load_u32_le(std::span<const std::byte> in, std::size_t offset) {

    std::uint32_t res = 0;
    res |= static_cast<std::uint32_t>(in[offset + 0]);
    res |= static_cast<std::uint32_t>(in[offset + 1]) << 8;
    res |= static_cast<std::uint32_t>(in[offset + 2]) << 16;
    res |= static_cast<std::uint32_t>(in[offset + 3]) << 24;

    return res;
}


[[gnu::always_inline]] inline std::uint64_t load_u64_le(std::span<const std::byte> in, std::size_t offset) {

    std::uint64_t res = 0;
    res |= static_cast<std::uint64_t>(in[offset + 0]);
    res |= static_cast<std::uint64_t>(in[offset + 1]) << 8;
    res |= static_cast<std::uint64_t>(in[offset + 2]) << 16;
    res |= static_cast<std::uint64_t>(in[offset + 3]) << 24;
    res |= static_cast<std::uint64_t>(in[offset + 4]) << 32;
    res |= static_cast<std::uint64_t>(in[offset + 5]) << 40;
    res |= static_cast<std::uint64_t>(in[offset + 6]) << 48;
    res |= static_cast<std::uint64_t>(in[offset + 7]) << 56;

    return res;
}








inline void encode_header(const MessageHeader& h, std::span<std::byte> out) {
    store_u32_le(out, h.off_magic, h.magic);
    store_u16_le(out, h.off_version, h.version);
    store_u16_le(out, h.off_message_type, static_cast<std::uint16_t>(h.message_type));
    store_u16_le(out, h.off_payload_length, h.payload_length);
    store_u16_le(out, h.off_flags, h.flags);
    store_u64_le(out, h.off_sequence_numer, h.sequence_numer);
    store_u64_le(out, h.off_session_id, h.session_id);
    store_u32_le(out, h.off_reserved, h.reserved);
}


inline DecodeStatus decode_header(std::span<const std::byte> in, MessageHeader& out) {
    if (in.size() < kHeaderSize)
        return DecodeStatus::NeedMoreData;

    out.magic           = load_u32_le(in, out.off_magic);
    out.version         = load_u16_le(in, out.off_version);
    out.message_type    = static_cast<MessageType>(load_u16_le(in, out.off_message_type));
    out.payload_length  = load_u16_le(in, out.off_payload_length);
    out.flags           = load_u16_le(in, out.off_flags);
    out.sequence_numer  = load_u64_le(in, out.off_sequence_numer);
    out.session_id      = load_u64_le(in, out.off_session_id);
    out.reserved        = load_u32_le(in, out.off_reserved);

    if (out.magic != kMagic)
        return DecodeStatus::BadMagic;

    if (out.version != kVersion)
        return DecodeStatus::BadVersion;

    return DecodeStatus::Ok;
}


inline bool is_known_request_type(MessageType type) {
    switch (type) {
        case MessageType::NewOrder:
        case MessageType::CancelOrder:
        case MessageType::ModifyOrder:
        case MessageType::Heartbeat:
        case MessageType::Logout:
            return true;
        default:
            return false;
    }
}


inline bool is_known_response_type(MessageType type) {
    switch (type) {
        case MessageType::Accepted:
        case MessageType::Rejected:
        case MessageType::Cancelled:
        case MessageType::Modified:
        case MessageType::Trade:
            return true;
        default:
            return false;
    }
}


inline std::uint16_t expected_payload_size(MessageType type) {
    switch (type) {
        case MessageType::NewOrder:
        case MessageType::CancelOrder:
        case MessageType::ModifyOrder:
        case MessageType::Heartbeat:
        case MessageType::Logout:
            return kPayloadSize;
        default:
            return 0;
        }
}


inline DecodeStatus validate_header(const MessageHeader& h) {
    if (!is_known_request_type(h.message_type))
        return DecodeStatus::UnknownMessageType;

    const auto exepcted = expected_payload_size(h.message_type);
    if (h.payload_length != exepcted)
        return DecodeStatus::BadPayloadLength;

    return DecodeStatus::Ok;
}

inline DecodeStatus validate_request_header(const MessageHeader& h) {
    if (!is_known_request_type(h.message_type))
        return DecodeStatus::UnknownMessageType;

    if (h.payload_length != kPayloadSize)
        return DecodeStatus::BadPayloadLength;

    return DecodeStatus::Ok;
}

inline DecodeStatus validate_response_header(const MessageHeader& h) {
    if (!is_known_response_type(h.message_type))
        return DecodeStatus::UnknownMessageType;

    if (h.payload_length != kPayloadSize)
        return DecodeStatus::BadPayloadLength;

    return DecodeStatus::Ok;
}





inline std::size_t encode_new_order(const MessageHeader& header, const NewOrder& order, std::span<std::byte> out) {
    const std::size_t total_size = kHeaderSize + kPayloadSize;

    MessageHeader h = header;
    h.message_type = MessageType::NewOrder;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + order.off_id, order.client_order_id);
    store_u64_le(out, base + order.off_side, static_cast<std::uint64_t>(order.side));
    store_u64_le(out, base + order.off_price, order.price);
    store_u64_le(out, base + order.off_quantity, order.quantity);

    return total_size;
}


inline DecodeStatus decode_new_order(std::span<const std::byte> in, NewOrder& out) {
    if (in.size() < kHeaderSize + kPayloadSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + out.off_id);
    out.side            = static_cast<Side>(load_u64_le(in, base + out.off_side));
    out.price           = load_u64_le(in, base + out.off_price);
    out.quantity        = load_u64_le(in, base + out.off_quantity);

    return DecodeStatus::Ok;
}



inline std::size_t encode_cancel_order(const MessageHeader& header, const CancelOrder& order, std::span<std::byte> out) {
    const std::size_t total_size = kHeaderSize + kPayloadSize;

    MessageHeader h = header;
    h.message_type = MessageType::CancelOrder;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + order.off_id, order.client_order_id);

    return total_size;
}


inline DecodeStatus decode_cancel_order(std::span<const std::byte> in, CancelOrder& out) {
    if (in.size() < kHeaderSize + kPayloadSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + out.off_id);

    return DecodeStatus::Ok;
}


inline std::size_t encode_modify_order(const MessageHeader& header, const ModifyOrder& order, std::span<std::byte> out) {
    const std::size_t total_size = kHeaderSize + kPayloadSize;

    MessageHeader h = header;
    h.message_type = MessageType::ModifyOrder;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + order.off_id, order.client_order_id);
    store_u64_le(out, base + order.off_new_price, order.new_price);
    store_u64_le(out, base + order.off_new_quantity, order.new_quantity);

    return total_size;
}


inline DecodeStatus decode_modify_order(std::span<const std::byte> in, ModifyOrder& out) {
    if (in.size() < kHeaderSize + kPayloadSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + out.off_id);
    out.new_price       = load_u64_le(in, base + out.off_new_price);
    out.new_quantity    = load_u64_le(in, base + out.off_new_quantity);

    return DecodeStatus::Ok;
}


inline std::size_t encode_accepted(const MessageHeader& header, const Accepted& response, std::span<std::byte> out) {
    MessageHeader h = header;
    h.message_type = MessageType::Accepted;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + Accepted::off_client_order_id, response.client_order_id);
    store_u64_le(out, base + Accepted::off_order_handle, response.order_handle);
    store_u64_le(out, base + Accepted::off_reserved1, response.reserved1);
    store_u64_le(out, base + Accepted::off_reserved2, response.reserved2);

    return kFrameSize;
}


inline DecodeStatus decode_accepted(std::span<const std::byte> in, Accepted& out) {
    if (in.size() < kFrameSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + Accepted::off_client_order_id);
    out.order_handle    = load_u64_le(in, base + Accepted::off_order_handle);
    out.reserved1       = load_u64_le(in, base + Accepted::off_reserved1);
    out.reserved2       = load_u64_le(in, base + Accepted::off_reserved2);

    return DecodeStatus::Ok;
}


inline std::size_t encode_rejected(const MessageHeader& header, const Rejected& response, std::span<std::byte> out) {
    MessageHeader h = header;
    h.message_type = MessageType::Rejected;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + Rejected::off_client_order_id, response.client_order_id);
    store_u64_le(out, base + Rejected::off_reason,
                 static_cast<std::uint64_t>(response.reason));
    store_u64_le(out, base + Rejected::off_reserved1, response.reserved1);
    store_u64_le(out, base + Rejected::off_reserved2, response.reserved2);

    return kFrameSize;
}


inline DecodeStatus decode_rejected(std::span<const std::byte> in, Rejected& out) {
    if (in.size() < kFrameSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + Rejected::off_client_order_id);
    out.reason          = static_cast<RejectReason>(load_u64_le(in, base + Rejected::off_reason));
    out.reserved1       = load_u64_le(in, base + Rejected::off_reserved1);
    out.reserved2       = load_u64_le(in, base + Rejected::off_reserved2);

    return DecodeStatus::Ok;
}


inline std::size_t encode_cancelled(const MessageHeader& header, const Cancelled& response, std::span<std::byte> out) {
    MessageHeader h = header;
    h.message_type = MessageType::Cancelled;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + Cancelled::off_client_order_id, response.client_order_id);
    store_u64_le(out, base + Cancelled::off_order_handle, response.order_handle);
    store_u64_le(out, base + Cancelled::off_reserved1, response.reserved1);
    store_u64_le(out, base + Cancelled::off_reserved2, response.reserved2);

    return kFrameSize;
}


inline DecodeStatus decode_cancelled(std::span<const std::byte> in, Cancelled& out) {
    if (in.size() < kFrameSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + Cancelled::off_client_order_id);
    out.order_handle    = load_u64_le(in, base + Cancelled::off_order_handle);
    out.reserved1       = load_u64_le(in, base + Cancelled::off_reserved1);
    out.reserved2       = load_u64_le(in, base + Cancelled::off_reserved2);

    return DecodeStatus::Ok;
}


inline std::size_t encode_modified(const MessageHeader& header, const Modified& response, std::span<std::byte> out) {
    MessageHeader h = header;
    h.message_type = MessageType::Modified;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + Modified::off_client_order_id, response.client_order_id);
    store_u64_le(out, base + Modified::off_order_handle, response.order_handle);
    store_u64_le(out, base + Modified::off_reserved1, response.reserved1);
    store_u64_le(out, base + Modified::off_reserved2, response.reserved2);

    return kFrameSize;
}


inline DecodeStatus decode_modified(std::span<const std::byte> in, Modified& out) {
    if (in.size() < kFrameSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + Modified::off_client_order_id);
    out.order_handle    = load_u64_le(in, base + Modified::off_order_handle);
    out.reserved1       = load_u64_le(in, base + Modified::off_reserved1);
    out.reserved2       = load_u64_le(in, base + Modified::off_reserved2);

    return DecodeStatus::Ok;
}


inline std::size_t encode_trade(const MessageHeader& header, const Trade& response, std::span<std::byte> out) {
    MessageHeader h = header;
    h.message_type = MessageType::Trade;
    h.payload_length = kPayloadSize;

    encode_header(h, out);

    const std::size_t base = kHeaderSize;
    store_u64_le(out, base + Trade::off_client_order_id, response.client_order_id);
    store_u64_le(out, base + Trade::off_order_handle, response.order_handle);
    store_u64_le(out, base + Trade::off_price, response.price);
    store_u64_le(out, base + Trade::off_quantity, response.quantity);

    return kFrameSize;
}


inline DecodeStatus decode_trade(std::span<const std::byte> in, Trade& out) {
    if (in.size() < kFrameSize)
        return DecodeStatus::NeedMoreData;

    const std::size_t base = kHeaderSize;

    out.client_order_id = load_u64_le(in, base + Trade::off_client_order_id);
    out.order_handle    = load_u64_le(in, base + Trade::off_order_handle);
    out.price           = load_u64_le(in, base + Trade::off_price);
    out.quantity        = load_u64_le(in, base + Trade::off_quantity);

    return DecodeStatus::Ok;
}

}
