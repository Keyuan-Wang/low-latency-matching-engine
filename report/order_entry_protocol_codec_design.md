# Order Entry Protocol Codec Design

## Goal

This is the first step of the order-entry gateway track.

The goal is not to implement TCP yet. The goal is to define a small binary protocol and a codec layer that can translate between:

```text
C++ message objects <-> wire-format bytes
```

The socket layer will later read bytes from TCP and pass them into this codec. Keeping the codec independent from sockets makes the protocol easy to test before adding nonblocking I/O, `epoll`, session state, and backpressure.

## Design Summary

The current design uses:

- a 32-byte message header;
- fixed-size order request payloads;
- explicit little-endian integer encoding;
- manual field offsets instead of `memcpy(struct)`;
- `std::span<std::byte>` as the buffer interface;
- no dynamic allocation on the encode/decode path.

The order-entry request frame is designed to be cache-line friendly:

```text
Header  32 bytes
Payload 32 bytes
Total   64 bytes
```

`NewOrder`, `CancelOrder`, and `ModifyOrder` all use a 32-byte payload. `CancelOrder` and `ModifyOrder` reserve unused fields instead of shrinking the frame. This wastes a few bytes, but gives the parser and future SPSC command path a predictable 64-byte order request layout.

## Header Layout

The protocol header is logically represented by `MessageHeader`, but the C++ struct layout is not the wire layout. The wire layout is defined by explicit offsets:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `magic` |
| 4 | 2 | `version` |
| 6 | 2 | `message_type` |
| 8 | 2 | `payload_length` |
| 10 | 2 | `flags` |
| 12 | 8 | `sequence_number` |
| 20 | 8 | `session_id` |
| 28 | 4 | `reserved` |

This is why the codec writes fields one by one:

```cpp
store_u64_le(out, MessageHeader::off_sequence_numer, h.sequence_numer);
```

The offset is the wire-buffer offset, not `offsetof(MessageHeader, sequence_numer)`. This avoids C++ padding and ABI issues.

## Payload Layout

### NewOrder

`NewOrder` uses four 8-byte fields:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `client_order_id` |
| 8 | 8 | `side` |
| 16 | 8 | `price` |
| 24 | 8 | `quantity` |

`Side` is encoded as a `uint64_t`, even though it only has two valid values. Since the order request payload is fixed at 32 bytes anyway, using an 8-byte side field avoids manual `u8 + padding` handling and keeps the payload as four uniform 64-bit loads/stores.

### CancelOrder

`CancelOrder` only needs `client_order_id`, but it is padded to 32 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `client_order_id` |
| 8 | 24 | reserved |

### ModifyOrder

`ModifyOrder` needs 24 bytes of data and 8 bytes of reserved space:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `client_order_id` |
| 8 | 8 | `new_price` |
| 16 | 8 | `new_quantity` |
| 24 | 8 | reserved |

## Why Explicit Little-Endian Helpers

The codec uses helpers such as:

```cpp
store_u64_le(...)
load_u64_le(...)
```

This makes the wire format explicit. The protocol does not depend on host endianness, compiler padding, or C++ object layout.

For example, `store_u16_le()` writes the low byte first:

```text
0x1234 -> 34 12
```

This is the protocol-level byte order. It is separate from how a C++ compiler stores fields inside a struct.

## Codec Boundary

The current codec layer is responsible for:

- encoding headers;
- decoding headers;
- checking magic/version;
- checking whether the message type is known;
- checking whether `payload_length` matches the expected fixed size;
- encoding and decoding `NewOrder`.

It deliberately does not handle:

- TCP reads or writes;
- partial frame buffering;
- session sequence validation;
- `client_order_id -> OrderHandle` lookup;
- duplicate order rejection;
- matching-engine calls.

Those are later gateway/session responsibilities.

## Why This Shape Fits The Project

The matching core is already optimized around predictable memory access and low instruction count. The protocol follows the same philosophy:

- fixed offsets instead of flexible field lookup;
- 64-byte order request frame instead of variable-length request bodies;
- no allocation while parsing;
- explicit status values instead of exceptions;
- codec separated from socket I/O so malformed-message tests can be written without a network.

This keeps the future network path simple:

```text
socket read
-> session input buffer
-> frame parser
-> codec
-> gateway validation
-> SPSC command queue / matching thread
```

## Current Limitations

This is still the codec stage.

The next missing pieces are:

- stronger round-trip tests for every request type;
- tests for bad magic, bad version, unknown message type, and bad payload length;
- side validation (`Buy` or `Sell` only);
- encode/decode support for cancel, modify, heartbeat, logout, and responses;
- a frame parser that can handle partial TCP reads and multiple messages in one buffer.

Once the frame parser exists, the project can move on to a blocking TCP echo/protocol server, then to nonblocking `epoll`.
