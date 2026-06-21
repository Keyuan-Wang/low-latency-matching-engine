#include "order_entry/codec.hpp"
#include "order_entry/frame_parser.hpp"
#include "order_entry/protocol.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

using namespace llmes::order_entry;

namespace {

using Frame = std::array<std::byte, kFrameSize>;
using Parser = FrameParser<128>;

std::span<const std::byte> as_span(const Frame& frame) {
    return {frame.data(), frame.size()};
}

Frame make_new_order_frame(std::uint64_t id,
                           Side side = Side::Buy,
                           std::uint64_t price = 10000,
                           std::uint64_t qty = 10,
                           std::uint64_t seq = 1,
                           std::uint64_t session = 42) {
    Frame frame{};

    MessageHeader header;
    header.sequence_numer   = seq;
    header.session_id       = session;

    NewOrder order;
    order.client_order_id   = id;
    order.side              = side;
    order.price             = price;
    order.quantity          = qty;

    const auto n = encode_new_order(header, order, frame);
    assert(n == kFrameSize);

    return frame;
}

Frame make_cancel_order_frame(std::uint64_t id,
                              std::uint64_t seq = 1,
                              std::uint64_t session = 42) {
    Frame frame{};

    MessageHeader header;
    header.sequence_numer = seq;
    header.session_id = session;

    CancelOrder order;
    order.client_order_id = id;

    const auto n = encode_cancel_order(header, order, frame);
    assert(n == kFrameSize);

    return frame;
}

Frame make_modify_order_frame(std::uint64_t id,
                              std::uint64_t new_price,
                              std::uint64_t new_qty,
                              std::uint64_t seq = 1,
                              std::uint64_t session = 42) {
    Frame frame{};

    MessageHeader header;
    header.sequence_numer = seq;
    header.session_id = session;

    ModifyOrder order;
    order.client_order_id = id;
    order.new_price = new_price;
    order.new_quantity = new_qty;

    const auto n = encode_modify_order(header, order, frame);
    assert(n == kFrameSize);

    return frame;
}

Frame make_control_frame(MessageType type,
                         std::uint64_t seq = 1,
                         std::uint64_t session = 42) {
    Frame frame{};

    MessageHeader header;
    header.message_type = type;
    header.payload_length = kPayloadSize;
    header.sequence_numer = seq;
    header.session_id = session;

    encode_header(header, frame);
    return frame;
}



void test_codec_new_order_round_trip() {
    const auto frame = make_new_order_frame(1001, Side::Sell, 12345, 99, 7, 88);

    MessageHeader header;
    assert(decode_header(frame, header) == DecodeStatus::Ok);
    assert(validate_header(header) == DecodeStatus::Ok);

    assert(header.message_type == MessageType::NewOrder);
    assert(header.payload_length == kPayloadSize);
    assert(header.sequence_numer == 7);
    assert(header.session_id == 88);

    NewOrder order;
    assert(decode_new_order(frame, order) == DecodeStatus::Ok);

    assert(order.client_order_id == 1001);
    assert(order.side == Side::Sell);
    assert(order.price == 12345);
    assert(order.quantity == 99);
}

void test_codec_cancel_order_round_trip() {
    const auto frame = make_cancel_order_frame(2002, 8, 88);

    MessageHeader header;
    assert(decode_header(frame, header) == DecodeStatus::Ok);
    assert(validate_header(header) == DecodeStatus::Ok);
    assert(header.message_type == MessageType::CancelOrder);

    CancelOrder order;
    assert(decode_cancel_order(frame, order) == DecodeStatus::Ok);
    assert(order.client_order_id == 2002);
}

void test_codec_modify_order_round_trip() {
    const auto frame = make_modify_order_frame(3003, 11111, 42, 9, 88);

    MessageHeader header;
    assert(decode_header(frame, header) == DecodeStatus::Ok);
    assert(validate_header(header) == DecodeStatus::Ok);
    assert(header.message_type == MessageType::ModifyOrder);

    ModifyOrder order;
    assert(decode_modify_order(frame, order) == DecodeStatus::Ok);
    assert(order.client_order_id == 3003);
    assert(order.new_price == 11111);
    assert(order.new_quantity == 42);
}

void test_codec_control_frames() {
    {
        const auto frame = make_control_frame(MessageType::Heartbeat, 10, 88);

        MessageHeader header;
        assert(decode_header(frame, header) == DecodeStatus::Ok);
        assert(validate_header(header) == DecodeStatus::Ok);
        assert(header.message_type == MessageType::Heartbeat);
        assert(header.payload_length == kPayloadSize);
    }

    {
        const auto frame = make_control_frame(MessageType::Logout, 11, 88);

        MessageHeader header;
        assert(decode_header(frame, header) == DecodeStatus::Ok);
        assert(validate_header(header) == DecodeStatus::Ok);
        assert(header.message_type == MessageType::Logout);
        assert(header.payload_length == kPayloadSize);
    }
}

void test_bad_header_validation() {
    {
        auto frame = make_new_order_frame(1);
        frame[MessageHeader::off_magic] = std::byte{0x00};

        MessageHeader header;
        assert(decode_header(frame, header) == DecodeStatus::BadMagic);
    }

    {
        auto frame = make_new_order_frame(1);
        store_u16_le(frame, MessageHeader::off_version, static_cast<std::uint16_t>(999));

        MessageHeader header;
        assert(decode_header(frame, header) == DecodeStatus::BadVersion);
    }

    {
        auto frame = make_new_order_frame(1);
        store_u16_le(frame, MessageHeader::off_message_type, static_cast<std::uint16_t>(999));

        MessageHeader header;
        assert(decode_header(frame, header) == DecodeStatus::Ok);
        assert(validate_header(header) == DecodeStatus::UnknownMessageType);
    }

    {
        auto frame = make_new_order_frame(1);
        store_u16_le(frame, MessageHeader::off_payload_length, static_cast<std::uint16_t>(16));

        MessageHeader header;
        assert(decode_header(frame, header) == DecodeStatus::Ok);
        assert(validate_header(header) == DecodeStatus::BadPayloadLength);
    }
}

void test_parser_empty_and_single_frame() {
    Parser parser;
    DecodedMessage msg;

    assert(parser.try_parse(msg) == Parser::Status::NeedMoreData);

    const auto frame = make_new_order_frame(1001, Side::Buy, 12345, 10);
    assert(parser.append(as_span(frame)));

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::NewOrder);
    assert(msg.new_order.client_order_id == 1001);
    assert(msg.new_order.side == Side::Buy);
    assert(msg.new_order.price == 12345);
    assert(msg.new_order.quantity == 10);

    assert(parser.try_parse(msg) == Parser::Status::NeedMoreData);
}

void test_parser_partial_frame() {
    Parser parser;
    DecodedMessage msg;

    const auto frame = make_new_order_frame(1002, Side::Sell, 22222, 20);

    assert(parser.append(as_span(frame).first(32)));
    assert(parser.try_parse(msg) == Parser::Status::NeedMoreData);

    assert(parser.append(as_span(frame).subspan(32)));
    assert(parser.try_parse(msg) == Parser::Status::MessageReady);

    assert(msg.type == MessageType::NewOrder);
    assert(msg.new_order.client_order_id == 1002);
    assert(msg.new_order.side == Side::Sell);
    assert(msg.new_order.price == 22222);
    assert(msg.new_order.quantity == 20);
}

void test_parser_multiple_frames() {
    Parser parser;
    DecodedMessage msg;

    const auto f1 = make_new_order_frame(1);
    const auto f2 = make_cancel_order_frame(2);
    const auto f3 = make_modify_order_frame(3, 33333, 30);

    assert(parser.append(as_span(f1)));
    assert(parser.append(as_span(f2)));

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::NewOrder);
    assert(msg.new_order.client_order_id == 1);

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::CancelOrder);
    assert(msg.cancel_order.client_order_id == 2);

    assert(parser.try_parse(msg) == Parser::Status::NeedMoreData);

    assert(parser.append(as_span(f3)));
    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::ModifyOrder);
    assert(msg.modify_order.client_order_id == 3);
    assert(msg.modify_order.new_price == 33333);
    assert(msg.modify_order.new_quantity == 30);
}

void test_parser_control_frames() {
    Parser parser;
    DecodedMessage msg;

    const auto heartbeat = make_control_frame(MessageType::Heartbeat, 100);
    const auto logout = make_control_frame(MessageType::Logout, 101);

    assert(parser.append(as_span(heartbeat)));
    assert(parser.append(as_span(logout)));

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::Heartbeat);
    assert(msg.header.sequence_numer == 100);

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::Logout);
    assert(msg.header.sequence_numer == 101);
}

void test_parser_bad_frame_does_not_consume() {
    Parser parser;
    DecodedMessage msg;

    auto frame = make_new_order_frame(1);
    frame[MessageHeader::off_magic] = std::byte{0x00};

    assert(parser.append(as_span(frame)));
    assert(parser.size() == kFrameSize);

    assert(parser.try_parse(msg) == Parser::Status::ProtocolError);

    // Bad frame remains at the front. Session layer can decide to close/reset.
    assert(parser.size() == kFrameSize);
    assert(parser.try_parse(msg) == Parser::Status::ProtocolError);
}

void test_parser_buffer_full() {
    Parser parser;

    const auto f1 = make_new_order_frame(1);
    const auto f2 = make_new_order_frame(2);
    const auto f3 = make_new_order_frame(3);

    assert(parser.append(as_span(f1)));
    assert(parser.append(as_span(f2)));
    assert(parser.full());

    assert(!parser.append(as_span(f3)));
}

void test_parser_append_wrap() {
    Parser parser;
    DecodedMessage msg;

    const auto f1 = make_new_order_frame(1);
    const auto f2 = make_new_order_frame(2);
    const auto f3 = make_new_order_frame(3);

    // Move read/write to index 64.
    assert(parser.append(as_span(f1)));
    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.new_order.client_order_id == 1);

    // Put 32 bytes of f2 at [64, 96).
    assert(parser.append(as_span(f2).first(32)));
    assert(parser.try_parse(msg) == Parser::Status::NeedMoreData);

    // Append 48 bytes at write_idx=96:
    // - 32 bytes complete f2 at [96, 128)
    // - 16 bytes of f3 wrap to [0, 16)
    assert(parser.append(as_span(f2).subspan(32)));
    assert(parser.append(as_span(f3).first(16)));

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::NewOrder);
    assert(msg.new_order.client_order_id == 2);

    assert(parser.try_parse(msg) == Parser::Status::NeedMoreData);

    // Complete f3 at [16, 64).
    assert(parser.append(as_span(f3).subspan(16)));

    assert(parser.try_parse(msg) == Parser::Status::MessageReady);
    assert(msg.type == MessageType::NewOrder);
    assert(msg.new_order.client_order_id == 3);
}

} // namespace

int main() {
    test_codec_new_order_round_trip();
    test_codec_cancel_order_round_trip();
    test_codec_modify_order_round_trip();
    test_codec_control_frames();

    test_bad_header_validation();

    test_parser_empty_and_single_frame();
    test_parser_partial_frame();
    test_parser_multiple_frames();
    test_parser_control_frames();
    test_parser_bad_frame_does_not_consume();
    test_parser_buffer_full();
    test_parser_append_wrap();

    std::cout << "order_entry tests passed\n";
    return 0;
}