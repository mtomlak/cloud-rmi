# Cloud-RMI Protocol — Unified Specification (v1 + v2)

Status: Draft (intended long‑term stable reference)
Scope: Protocol framing, parsing, state machine, negotiation, extensibility
Out of scope: Business logic, scheduling, language bindings, transport internals

Use `docs/PROTOCOL_IMPLEMENTATION.md` for the authoritative, code-aligned
command, error, and capability reference.

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

- `PROTO=2` announces the maximum version; client remains on v1 until it sends
  `PROTO <version>`.
- Capability tokens are uppercase words (`PAYLOAD`, `PROXY`, `STATS`, vendor
  extensions) separated by commas in the banner and normalized to spaces in
  responses.
- Servers MUST advertise any non‑standard command or behavior as a capability.

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

- `MAX_LINE = 10240` bytes (derived from `RMI_BUF_SZ`)
- Each command line MUST contain `\n` within 10239 bytes
- Otherwise:

```
400 Bad Request
```

### 5.2 Buffer Ceilings

- Total buffered data MUST NOT exceed `RMI_MAX_BUFFER = RMI_BUF_SZ * 8`
- Overflow resets protocol state and produces `400 Bad Request`

### 5.3 Payload Policy Thresholds (Non‑normative)

Typical policy:

- Inline base64 payloads should avoid exceeding ~4k
- Larger payloads SHOULD use v2 payload framing

This is a **policy decision**, not a protocol rule. See the implementation
companion for exact buffering and overflow behaviors enforced by the parser.

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

State transitions are deterministic and protocol‑version aware.

---

## 7. Commands Overview (Summary)

Commands that NEVER accept payload:

```
AUTH
HELP
IFACES
NOOP
TIME
UPTIME
DEBUG
QUIT
EXIT
SERVER
PROXY
```

Servers MAY implement additional commands but MUST advertise them via banner
capabilities. Detailed semantics, authentication rules, and status codes live
in `docs/PROTOCOL_IMPLEMENTATION.md`.

---

## 8. CALL Command (Summary)

### 8.1 Protocol v1 (Legacy — Always Supported)

```
CALL <iface> <method> <base64-args>
```

### 8.2 Protocol v2 (Extended Framing)

Inline form remains valid. Payload form adds explicit framing:

```
CALL <iface> <method> <encoding> <length>
<raw payload bytes>
```

Where `<length>` is decimal byte count and `<encoding>` is a symbolic token
(`RAW`, `PHP`, `JSON`, etc.).

### 8.3 Disambiguation Rules

- 3 arguments → inline form
- 4 arguments → payload form (v2 only)
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
200 <encoding> <length>
<raw payload bytes>
```

or

```
500 <encoding> <length>
<raw payload bytes>
```

Rules:
- Server MUST NOT emit payload responses to v1 clients
- Server MUST NOT downgrade to inline if MAX_LINE would be exceeded
- Payload response framing must exactly match the announced length

---

## 11. Error Reporting Model (Summary)

- Transport / framing errors use explicit status codes (`400`, `401`, `500`)
- Semantic errors are reported via structured error payloads
- Parser errors return `RMI_ERROR_BAD_REQUEST` or `RMI_ERROR_BAD_ARGS`
- Overflow resets parser state to avoid desynchronization

Full error code mapping and recovery expectations are described in
`docs/PROTOCOL_IMPLEMENTATION.md`.

---

## 12. Handler argv Contract (Summary)

Handlers MUST receive stable argv layout:

```
argv[0] = file
argv[1] = method
argv[2] = encoding (PLAIN for inline)
argv[3] = args (decoded inline args, empty or "-" for payload)
```

Payload bytes are always read from stdin. See the companion document for the
exact lifecycle of payload pipes, exit-code handling, and newline trimming.

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
