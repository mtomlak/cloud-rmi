# Cloud-RMI Protocol — Unified Specification (v1 + v2)

Status: Draft (intended long‑term stable reference)
Scope: Protocol framing, parsing, state machine, negotiation, extensibility
Out of scope: Business logic, scheduling, language bindings, transport internals

Use `docs/IMPLEMENTATION.md` for implementation-specific details on how the
C server, client, and proxy currently behave.

---

## 1. Goals & Design Constraints

The Cloud‑RMI protocol is designed to:

- Preserve **full backward compatibility** with all existing v1 clients
- Allow **incremental evolution** via explicit opt‑in (v2 and later)
- Support **large, binary‑safe payloads**
- Avoid heuristic parsing and speculative reads
- Remain suitable for:
  - High‑performance C servers
  - PHP CLI / extension servers
  - Mixed‑language clients
- Remain stable for **10+ years**

Core principles:

1. Text‑based control plane
2. Explicit framing for payloads
3. Deterministic state machine
4. No implicit upgrades
5. Client controls argument encoding
6. Server controls response encoding
7. Encoding semantics are orthogonal to framing

---

## 2. Transport Model

The protocol is transport‑agnostic and assumes only:

- Ordered byte stream
- Reliable delivery

Examples:
- TCP sockets
- Unix domain sockets
- Pipes / IPC

---

## 3. Connection Lifecycle

```
CONNECT
  ↓
SERVER BANNER
  ↓
COMMAND LOOP
```

All semantics below apply inside the **COMMAND LOOP**.

---

## 4. Protocol Versioning & Negotiation

### 4.1 Default Mode

- Every connection **starts in protocol v1**
- v1 parsing and framing rules remain active until explicit upgrade
- Protocol version is **per‑connection state**

### 4.2 Banner & Capabilities

Server sends a banner:

```
220 <context> Server Ready EXT PROTO=2[,<capability-list>]
```

Banner grammar (authoritative):

- Status code `220` must appear at the start of the line.
- `<context>` is a single token matching `[A-Za-z0-9._-]+`.
- Decorative text between the context token and `EXT` is ignored by parsers.
- The negotiation block is `EXT PROTO=<version>[,<CAP1>[,<CAP2>...]]`.
- `PROTO=` MUST be the first item in the `EXT` block.
- Capability tokens are alphanumeric with `_`/`-`, are case-insensitive, and
  are normalized to uppercase. Order is not semantically relevant.

- `PROTO=2` announces the maximum version; client remains on v1 until it sends
  `PROTO <version>`.
- Capability tokens are uppercase words (`PROXY`, `STATS`, vendor
  extensions) separated by commas in the banner and normalized to spaces in
  responses.
- Servers MUST advertise any non-standard command or behavior as a capability.

### 4.3 Explicit Upgrade

Client upgrades protocol:

```
PROTO 2
```

Server replies:

```
200 OK
```

Rules:

- No implicit upgrades
- No mid‑command upgrades
- If client never sends `PROTO`, connection remains v1 forever

---

## 5. Framing & Limits

### 5.1 Line Framing

- `MAX_LINE = 8192` bytes (derived from `RMI_BUF_SZ`)
- Each command line MUST contain `\n` within 8192 bytes
- Otherwise:

```
400 Bad Request
```

### 5.2 Payload ceilings (v2 framing)

- **MAX_LINE** applies only to command lines and headers.
- **MAX_PAYLOAD** is a configurable safety limit (default 10 MB, per endpoint).
- Servers and clients **MUST** reject framed payloads exceeding their configured
  `MAX_PAYLOAD` and either drain the bytes or close cleanly to avoid
  desynchronization.

### 5.3 Payload Policy Thresholds (Non-normative)

Default policy:

- Inline base64 payloads are gated by
  `RMI_INLINE_PAYLOAD_THRESHOLD = RMI_MAX_LINE - floor(RMI_MAX_LINE * 0.35) - 500`
  (4,825 bytes with the current limits) **before** any base64 encoding.
- Calls below that soft limit still validate the fully framed, base64-encoded
  line against `RMI_MAX_LINE`; if the encoded line is `>= RMI_MAX_LINE`, the
  client MUST fall back to payload framing.
- Larger payloads SHOULD use v2 payload framing without performing the initial
  base64 conversion.

This is a **policy decision**, not a protocol rule. See the implementation
companion for exact buffering and overflow behaviors enforced by the parser.

#### Legacy v1 compatibility (PHP client)

- Protocol v1 has no strict `MAX_LINE` guarantee on responses; the PHP client
  accepts inline responses up to a **defensive 256 KB cap**. Larger responses
  MUST use payload framing via protocol v2.

---

## 6. Protocol States

The protocol is defined by a strict state machine.

### States

| State | Description |
|-----|------------|
| IDLE | Waiting for a command line |
| READING_PAYLOAD | Reading fixed‑length payload bytes |
| EXECUTE_CALL | Decoding, dispatching method |
| SEND_RESPONSE | Emitting response |
| CLOSED | Connection closed |

State transitions are deterministic and protocol‑version aware. Each request is
announced by exactly one newline‑terminated control line; in protocol v2 the
payload framing metadata lives on that CALL line.

---

## 7. Commands Overview (Summary)

Commands that NEVER accept payload:

```
AUTH
HELP
IFACES
INFO
PING
TIME
QUIT
EXIT
SERVER
PROXY
```

Servers MAY implement additional commands but MUST advertise them via banner
capabilities. Detailed semantics, authentication rules, and status codes live
in `docs/IMPLEMENTATION.md`.

---

## 8. CALL Command (Summary)

### 8.1 Protocol v1 (Legacy — Always Supported)

```
CALL <iface> <method> <base64-args>
```

### 8.2 Protocol v2 (Extended Framing)

Inline form remains valid. Payload framing is carried on the CALL line itself:

```
CALL <iface> <method> PAYLOAD <encoding> <length>
<raw payload bytes>
```

Where `<length>` is a decimal byte count and `<encoding>` is a symbolic token
(`RAW`, `PHP`, `JSON`, etc.). The `PAYLOAD` keyword is never a standalone
command; payload bytes start immediately after the newline terminating the CALL
line and end exactly after `<length>` bytes with no trailing delimiter. This
form is valid only after successful `PROTO 2` negotiation.

### 8.3 Disambiguation Rules

- `PAYLOAD` keyword in the third CALL position → payload form (v2 only)
- Three CALL arguments → inline form
- Encoding tokens MUST be from a fixed identifier alphabet and MUST NOT collide
  with base64

### 8.4 Payload Read Semantics

- Server reads **exactly `<length>` bytes**
- Payload may contain arbitrary binary data (including `\0`)
- Zero‑length payload is valid
- Server MUST NOT read past `<length>`

See the implementation companion for handler argv contract, error mapping, and
response formatting rules derived from the C server and proxy.

---

## 9. CALL Execution Semantics (Summary)

During execution:

1. Payload is decoded according to encoding
2. Arguments are hydrated
3. Method is invoked
4. Result or exception is produced

Rules:

- Client chooses argument encoding
- Server chooses response encoding

---

## 10. Responses (Summary)

### 10.1 Legacy Inline Response

```
200 <encoding>:<base64>
500 <encoding>:<base64>
```

### 10.2 Payload Response (v2)

```
200 PAYLOAD <encoding> <length>
<raw payload bytes>
```

or

```
500 PAYLOAD <encoding> <length>
<raw payload bytes>
```

Rules:
- Server MUST NOT emit payload responses to v1 clients
- Server MUST NOT downgrade to inline if MAX_LINE would be exceeded
- Payload response framing must exactly match the announced length

### 10.3 Response Line Rules and JSON Marker

- Every response is exactly **one line**. Control/status text and JSON payloads
  must not embed newlines; binary data always rides the explicit PAYLOAD
  channel rather than being inlined.
- Plaintext status and control keywords remain plaintext (`200 OK`, `400 Bad
  Request`, `200 PAYLOAD ...`). Do **not** wrap protocol directives in JSON.
- Structured responses use the explicit `JSON` marker and keep the JSON string
  as the final token: `200 JSON <json>`. JSON is emitted on a single line
  (compact form, no pretty printing).
- Enumerations are JSON arrays (e.g., HELP/IFACES/CAPS → `200 JSON ["AUTH",…]`)
  while structured/named data is a JSON object. Avoid wrapping arrays in an
  object “for future extensibility”; switch to an object only when new fields
  are required.
- When payload responses are converted to inline form for legacy clients (for
  example, by a proxy), keep the encoding token as a prefix before the base64
  payload (`<encoding>:<base64>`).

---

## 11. Error Reporting Model (Summary)

- Transport / framing errors use explicit status codes (`400`, `401`, `500`)
- Semantic errors are reported via structured error payloads
- Parser errors return `RMI_ERROR_BAD_REQUEST` or `RMI_ERROR_BAD_ARGS`
- Overflow resets parser state to avoid desynchronization

Full error code mapping and recovery expectations are described in
`docs/IMPLEMENTATION.md`.

---

## 12. Handler argv Contract (Summary)

Handlers MUST receive stable argv layout:

```
argv[0] = file
argv[1] = method
argv[2...] = inline args parsed with POSIX shell-style quoting
```

For payload calls, `argv[2]` carries the encoding token (empty string allowed)
and the raw payload bytes are always read from stdin; no synthetic argument is
inserted. Inline calls preserve empty quoted tokens when splitting the decoded
argument string. See the companion document for the exact lifecycle of payload
pipes, exit-code handling, and newline trimming.

---

## 13. Proxy Semantics (Summary)

- Proxy is a full protocol endpoint on both sides
- Maintains independent protocol states:
  - client ↔ proxy
  - proxy ↔ backend
- Proxy MAY translate inline ↔ payload when bridging v1/v2 clients
- Proxy MUST fully consume payload before forwarding

Detailed pooling, keepalive, and response adaptation rules are documented in the
implementation companion.

---

## 14. Extensibility & Vendor Commands

- Servers MAY implement additional commands
- Such commands MUST:
  - Be advertised via banner capability
  - Remain version‑scoped
  - Not alter base protocol semantics

Asynchronous execution models (e.g. forked execution) are **server‑internal
behavior** and outside protocol scope.

---

## 15. Backward Compatibility Guarantees

- v1 remains supported indefinitely
- v2 is opt‑in only
- No framing changes without `PROTO 2`
- Inline CALL semantics remain valid forever

---

## 16. Summary

- v1 is stable and permanent
- v2 introduces explicit payload framing
- Upgrade is explicit and deterministic
- Protocol remains simple, evolvable, and proxy‑safe

This document is intended to remain valid for the next decade. See
`docs/PROTOCOL_IMPLEMENTATION.md` for code-level details and error mappings.
