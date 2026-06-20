# Order Entry Protocol And Frame Parser Design

## Goal

This is the first step of the order-entry gateway track: define a small binary
order-entry protocol and a parser that can turn TCP bytes into complete request
messages.

The goal is not to build a generic TCP demo. The goal is to build the boundary
that a real matching engine would expose to clients:

```text
TCP byte stream -> fixed-size protocol frames -> decoded request messages
```

The codec still stays independent from sockets. The frame parser owns the
per-session byte buffer and handles partial reads. The future socket layer will
read from nonblocking TCP and append bytes into this parser.

## Design Summary

The current design uses:

- a 32-byte message header;
- a fixed 32-byte payload for every request type;
- a fixed 64-byte wire frame;
- explicit little-endian integer encoding;
- manual field offsets instead of `memcpy(struct)`;
- `std::span<std::byte>` as the buffer interface;
- no dynamic allocation on the encode/decode or frame-parse path.

Every request frame has the same shape:

```text
Header  32 bytes
Payload 32 bytes
Total   64 bytes
```

`NewOrder`, `CancelOrder`, `ModifyOrder`, `Heartbeat`, and `Logout` all use the
same 64-byte wire frame. Some messages do not need 32 bytes of business payload,
so unused bytes are reserved and should be zero-filled by the encoder.

This wastes a few bytes for small control messages, but removes variable-length
framing from the hot parser path.

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

## Request Payload Layout

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

`CancelOrder` only needs `client_order_id`, but it still uses a 32-byte payload:

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

### Heartbeat And Logout

`Heartbeat` and `Logout` are protocol control messages. They still carry the
standard header and a 32-byte reserved payload:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 32 | reserved |

This keeps every message at 64 bytes. The header still carries the important
metadata: magic, version, message type, payload length, sequence number, and
session id.

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
- checking whether the request message type is known;
- checking whether `payload_length` is the fixed 32-byte payload size;
- encoding and decoding request payloads.

It deliberately does not handle:

- TCP reads or writes;
- session sequence validation;
- `client_order_id -> OrderHandle` lookup;
- duplicate order rejection;
- matching-engine calls.

Those are later gateway/session responsibilities.

## Frame Parser Design

TCP is a byte stream. One `read()` can return half a header, one complete frame,
several frames, or a frame plus the beginning of the next one. The frame parser
exists to hide this from the gateway.

The parser is a per-session object:

```text
socket read bytes
-> FrameParser::append()
-> FrameParser::try_parse()
-> DecodedMessage
```

It uses an internal fixed-size ring buffer. `append()` accepts arbitrary byte
chunks from the socket and writes them into the ring. If the write crosses the
physical end of the array, it splits the copy into two `memcpy()` calls.

Parsing is simpler because the protocol frame size is fixed:

```text
if buffered bytes < 64:
    NeedMoreData
else:
    decode 32-byte header
    validate header
    decode payload based on message_type
    consume exactly 64 bytes
```

The parser never consumes a partial frame. It only advances `read_pos_` after a
complete frame has been decoded successfully.

## Why Fixed 64-Byte Frames Matter

The earlier variable-payload design made the parser depend on the header before
it knew how many bytes to consume. It also created an awkward wrap-around case:
a frame could start near the physical end of the ring buffer and continue at the
front.

The fixed-frame design avoids that.

`FrameParser<Capacity>` requires:

```text
Capacity is a power of two
Capacity % 64 == 0
```

Since every successful parse consumes exactly 64 bytes, frame starts remain
64-byte aligned inside the ring. A complete frame is therefore physically
contiguous in the ring buffer. The parser can return a plain 64-byte
`std::span<const std::byte>` without a temporary buffer on the parse path.

This gives the parser a very small hot path:

- one size check;
- one contiguous 64-byte span;
- header decode;
- request-type switch;
- one fixed `read_pos_ += 64`.

The only wrap-around logic left is in `append()`, because TCP can deliver any
number of bytes per read. That is fine: socket reads may be partial, but protocol
frames are consumed in fixed 64-byte units.

## Threading Boundary

`FrameParser` is intentionally not a two-thread data structure. It is a
single-threaded per-session parser owned by the I/O thread.

The intended future pipeline is:

```text
I/O thread:
  epoll -> read socket -> FrameParser -> lightweight protocol validation

SPSC queue:
  parsed command transfer

matching thread:
  gateway logic -> matching engine
```

The SPSC queue belongs after parsing, not inside the parser. Passing raw TCP
bytes across threads would make partial-frame state concurrent and would add
cache-line traffic for very little gain. Passing complete decoded commands is a
cleaner boundary.

## Why This Shape Fits The Project

The matching core is already optimized around predictable memory access and low instruction count. The protocol follows the same philosophy:

- fixed offsets instead of flexible field lookup;
- 64-byte request frames instead of variable-length request bodies;
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

## Current Limitations And Next Steps

This is still before the real TCP server.

The next missing pieces are:

- stronger round-trip tests for every request type;
- tests for bad magic, bad version, unknown message type, and bad payload length;
- side validation (`Buy` or `Sell` only);
- tests for partial reads, multiple frames in one buffer, and ring-buffer write wrap;
- response encoding support (`Accepted`, `Rejected`, `Cancelled`, `Modified`, `Trade`);
- sequence-number validation at the session layer;
- duplicate order rejection and unknown cancel rejection at the gateway layer.

After these tests are in place, the project can move to a blocking protocol
server first, then to nonblocking `epoll`, per-session input/output buffers, and
backpressure.
