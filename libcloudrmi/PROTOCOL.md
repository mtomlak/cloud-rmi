# CloudRMI Protocol v2 — PAYLOAD-aware Design Specification

**Status:** Draft (intended as long-term stable reference)  
**Scope:** Protocol framing, parsing, state machine, backward compatibility  
**Out of scope:** Business logic, language bindings, transport implementation

---

## 1. Goals & Design Constraints

This protocol revision is designed to:

- Preserve **full backward compatibility** with existing CloudRMI clients/servers
- Support **large and binary payloads** efficiently
- Avoid heuristic or ambiguous parsing
- Be suitable for:
  - C servers (high-performance, prefork, IPC)
  - PHP servers (CLI, extension-backed)
  - Mixed-language clients
- Remain stable for **10+ years**

Key principles:

1. **Text-based control plane**
2. **Explicit binary payload plane**
3. **No speculative reads**
4. **Deterministic state machine**
5. **Client controls argument encoding**
6. **Server controls response encoding**

---

## 2. Transport Model

The protocol is **transport-agnostic**:

- TCP sockets
- Unix domain sockets
- IPC / pipes

The protocol itself assumes:

- Ordered byte stream
- Reliable delivery

---

## 3. High-level Protocol Flow

```
CONNECT
  ↓
SERVER BANNER
  ↓
COMMAND LOOP
```

All semantics below apply inside the **COMMAND LOOP**.

---

## 4. Protocol States

The server processes input using a strict state machine.

### States

| State | Description |
|-----|------------|
| `IDLE` | Waiting for a new command line |
| `CALL_PENDING_PAYLOAD` | CALL accepted, waiting for PAYLOAD header |
| `READING_PAYLOAD` | Reading binary payload of known length |
| `EXECUTE_CALL` | Decoding arguments and invoking method |
| `SEND_RESPONSE` | Sending response (inline or PAYLOAD) |

---

## 5. Commands Overview

### Commands that **never** accept payload

Handled immediately in `IDLE` state:

```
AUTH
HELP
IFACES
NOOP
TIME
UPTIME
DEBUG
QUIT
SERVER
PROXY
```

If these commands are followed by a PAYLOAD, it is a protocol error.

---

## 6. CALL Command — Two Forms

### 6.1 Legacy CALL (backward compatible)

```
CALL <iface> <method> <inline_payload>
```

- `<inline_payload>` is a **single token**
- Typically base64, `PHP:base64(...)`, etc.
- Parsed and executed immediately

State transition:

```
IDLE → EXECUTE_CALL
```

This form **must remain supported indefinitely**.

---

### 6.2 Extended CALL (PAYLOAD-capable)

```
CALL <iface> <method>
```

Characteristics:

- Exactly **2 arguments**
- No payload inline
- Execution is deferred
- Server validates:
  - authentication
  - interface existence
  - method existence

State transition:

```
IDLE → CALL_PENDING_PAYLOAD
```

---

## 7. PAYLOAD Command

### 7.1 PAYLOAD Header

Valid **only** in `CALL_PENDING_PAYLOAD` state.

```
PAYLOAD <encoding> <length>
```

Where:

- `<encoding>`: symbolic encoding name (`PLAIN`, `PHP`, `JSON`, `BLOB`, etc.)
- `<length>`: exact byte count of payload to follow

Validation rules:

- PAYLOAD is **illegal** without a preceding CALL
- `<length>` must be ≤ configured maximum
- `<encoding>` must be supported

State transition:

```
CALL_PENDING_PAYLOAD → READING_PAYLOAD
```

---

### 7.2 PAYLOAD Data

Immediately following the PAYLOAD header:

```
<raw binary bytes, exactly <length>>
```

Rules:

- No line parsing
- No terminators required
- Binary-safe
- Server reads exactly `<length>` bytes

On completion:

```
READING_PAYLOAD → EXECUTE_CALL
```

---

## 8. Invalid PAYLOAD Scenarios

| Situation | Server Response |
|--------|----------------|
| PAYLOAD without CALL | `400 Unexpected PAYLOAD` |
| CALL already pending | `400 CALL already pending` |
| Any other command in `CALL_PENDING_PAYLOAD` | `400 Expected PAYLOAD` |
| Payload too large | `413 Payload Too Large` (or `400`) |
| Payload decode error | `400 Bad Payload` |

After error:
- Payload buffer is discarded
- State resets to `IDLE`

---

## 9. CALL Execution Semantics

During `EXECUTE_CALL`:

1. Payload (inline or PAYLOAD) is decoded
2. Arguments are hydrated
3. Method is invoked
4. Result or exception is produced

Encoding rules:

- **Client chooses encoding for arguments**
- **Server chooses encoding for result**

---

## 10. Responses

### 10.1 Inline Response (legacy-compatible)

```
200 <encoded-payload>
```

Used when:
- Payload is small
- Client compatibility is required

---

### 10.2 PAYLOAD Response (extended)

```
200 PAYLOAD <encoding> <length>
<raw bytes>
```

Notes:

- Same PAYLOAD semantics as request side
- Binary-safe
- Length is exact

Server may downgrade to inline form if needed.

State transition:

```
SEND_RESPONSE → IDLE
```

---

## 11. Error Reporting Model

Two independent channels:

### 11.1 Transport / Framing Errors
Reported via protocol status:

- `RMI_E_AGAIN`
- `RMI_E_OVERFLOW`
- `RMI_E_ERR`

These indicate stream-level issues.

### 11.2 Semantic / Protocol Errors
Reported via `err->code`:

- `RMI_ERROR_BAD_REQUEST`
- `RMI_ERROR_BAD_ARGS`
- `RMI_ERROR_UNAUTHORIZED`
- etc.

A syntactically valid event may still carry a semantic error.

---

## 12. Backward Compatibility Guarantees

- Old clients using legacy CALL continue to work unchanged
- PAYLOAD is opt-in and explicitly advertised
- No existing command syntax is broken
- Inline payload semantics remain valid

---

## 13. Encoding Notes

Encodings are **payload semantics**, not protocol framing.

Examples:

- `PLAIN` — base64 or raw plaintext fallback
- `PHP` — PHP `serialize()` + base64 (legacy)
- `JSON` — JSON + base64
- `BLOB` — raw binary

Future encodings may be added without protocol changes.

---

## 14. Rationale (Why This Design)

- Eliminates line-length limitations for arguments
- Avoids scanning payload content for magic bytes
- Allows fixed-size buffers for command parsing
- Cleanly separates:
  - framing
  - decoding
  - execution
- Maps directly to feed/emit engines
- Matches patterns used in mature RPC protocols

---

## 15. Implementation Notes (Non-normative)

- PAYLOAD should only be allowed immediately after CALL
- Protocol engine should be reusable from:
  - C server
  - PHP extension
  - Test harness
- Default max payload size must be configurable
- Banner should advertise PAYLOAD support explicitly (e.g. `EXT PAYLOAD`)

---

## 16. Summary

This protocol design:

- Preserves legacy behavior
- Introduces explicit payload framing
- Scales to large and binary data
- Is simple enough to reason about formally
- Avoids accidental complexity

**This document is intended to remain valid for the next decade.**


---

update for v2:


# Cloud-RMI Protocol v2 – Framing, Negotiation, and Proxy Semantics

## 1. Line Length & Framing Limits

### Hard limits
- MAX_LINE = 8192 bytes  
- Any command line must contain '\n' within 8191 bytes  
- Otherwise: 400 Bad Request
-

### Client policy thresholds (soft)
- PAYLOAD_INLINE_AVOID_B64 = 4096 bytes  
- If arguments or results would exceed ~4k after base64, prefer payload framing  
- This is a client/server policy, not a protocol constraint

---

## 2. Protocol Versioning & Negotiation

### Default behavior
- Every new connection starts in legacy mode (protocol v1)
- Server must not change parsing or framing rules until client explicitly opts in

### Banner
Server advertises maximum supported protocol:

220 <context> Server Ready EXT PROTO=2,STATS,PROXY

### Client opt-in
Client explicitly upgrades:

PROTO 2

Server responds:

200 OK

### Rules
- If client never sends PROTO, connection remains protocol v1 forever
- Protocol version is per-connection state
- No implicit or mid-command upgrades

---

## 3. CALL Command Formats

### Protocol v1 (legacy)

CALL <iface> <method> <base64-args>

---

### Protocol v2

We should prefer faster, inline form, until the encoded payload is larger than MAX_LINE minus some safety
threshold of cca 500 bytes for CALL symbol, interface name, method name etc..

To save time we should do educated guess how long is base64 gonna be - usually is like 4 * ceil(N / 3)
i think, or similar, we can do more precise calculation. So, if educated guess of base64 is bigger than
MAX_LINE - 500, we should use payload form. Same, if we estimate base64 is smaller than threshold but 
after physical base64 encoding it indeed is bigger, we should use payload form.


#### Inline form (same as v1)

CALL <iface> <method> <base64-args>

#### Payload form (new)

CALL <iface> <method> <encoding> <length>
<raw payload bytes>

- encoding: RAW, PHP, JSON
- length: decimal byte count
- Payload follows immediately, no terminator



### Disambiguation
- 3 args → inline
- 4 args → payload



---

## 4. Response Formats

### Legacy

200 <encoding>:<base64>
500 <encoding>:<base64>

### Payload response (v2)

200 <encoding> <length>
<raw payload bytes>

500 <encoding> <length>
<raw payload bytes>

---

## 5. Proxy Semantics

- Proxy is a full protocol endpoint on both sides
- Maintains independent protocol states:
  - client ↔ proxy
  - proxy ↔ backend

### Translation allowed
Proxy may translate inline ↔ payload as required.

If backend cannot safely represent request, proxy returns 400.

---

## 6. Client Behavior

- After banner, if PROTO 2 advertised → send PROTO 2
- Use inline when small
- Use payload when exceeding thresholds

---

## 7. Server Behavior

- Start in legacy mode
- Switch to v2 only after PROTO 2
- Never emit payload framing to legacy clients

---

## 8. Handler argv Contract (IMPORTANT)

Always keep argv stable:

argv[0] = file  
argv[1] = method  
argv[2] = encoding (PLAIN for inline)  
argv[3] = args (decoded args for inline, empty or '-' for payload)  

Payload bytes are always read from stdin.

---

## 9. Rationale

- Full backward compatibility
- Removes base64 overhead for large payloads
- Deterministic parsing
- Proxy-safe
- Clean evolution path
