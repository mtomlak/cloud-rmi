# CloudRMI Protocol v2 — Framing & PAYLOAD Metadata Specification

**Status:** Draft (stable framing layer)  
**Scope:** Command framing, PAYLOAD state machine, backward compatibility  
**Out of scope:** Encoding semantics and data interpretation

---

## 0. Clarification on Encoding

This protocol **does support declaring an encoding type** at the framing level,
but it **does not define what that encoding means**.

Key distinction:

- **Protocol layer**: transports bytes + declares metadata
- **Application layer**: interprets bytes based on encoding

This allows:
- clean framing in C
- PHP (or other languages) to keep their own encoding rules
- future encodings without protocol redesign

---

## 1. Goals & Constraints

- Preserve backward compatibility
- Support large / binary payloads
- Avoid line-length limits
- Deterministic parsing (no heuristics)
- Long-term stability (10+ years)

---

## 2. Transport Model

Transport-agnostic byte stream:
- TCP
- Unix sockets
- IPC

Assumes ordered, reliable delivery.

---

## 3. High-level Flow

```
CONNECT
  ↓
SERVER BANNER
  ↓
COMMAND LOOP
```

---

## 4. Protocol States

| State | Meaning |
|------|--------|
| `IDLE` | Waiting for a command line |
| `CALL_PENDING_PAYLOAD` | CALL accepted, waiting for PAYLOAD |
| `READING_PAYLOAD` | Reading fixed-size payload |
| `EXECUTE_CALL` | Call execution |
| `SEND_RESPONSE` | Sending response |

---

## 5. Commands Without Payload

Handled immediately in `IDLE`:

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

These commands **never accept PAYLOAD**.

---

## 6. CALL Command

### 6.1 Legacy CALL (inline args)

```
CALL <iface> <method> <args>
```

- `<args>` is a single token
- Treated as opaque bytes
- Executed immediately

State transition:

```
IDLE → EXECUTE_CALL
```

Must remain supported indefinitely.

---

### 6.2 Extended CALL (payload-based)

```
CALL <iface> <method>
```

Rules:
- Exactly 2 arguments
- No execution yet
- Server validates auth / iface / method

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

- `<encoding>` = symbolic identifier (e.g. `PLAIN`, `PHP`, `JSON`, `BLOB`)
- `<length>` = exact number of bytes to read next

Protocol rules:

- Encoding value is **opaque to the protocol**
- Protocol does not validate or interpret encoding
- Encoding string is forwarded to application layer

State transition:

```
CALL_PENDING_PAYLOAD → READING_PAYLOAD
```

---

### 7.2 PAYLOAD Body

Immediately follows header:

```
<exactly <length> raw bytes>
```

Properties:

- Binary-safe
- No terminator
- No inspection by protocol engine

After read:

```
READING_PAYLOAD → EXECUTE_CALL
```

---

## 8. Invalid Sequences

| Situation | Server Action |
|---------|---------------|
| PAYLOAD without CALL | 400 error |
| Second CALL before payload | 400 error |
| Any command instead of PAYLOAD | 400 error |
| Payload too large | Abort payload, reset |
| Short read | Abort payload, reset |

After any framing error:
- Payload buffer is discarded
- State resets to `IDLE`

---

## 9. Call Execution

- Server receives:
  - raw payload bytes
  - declared encoding string
- Decoding and validation are application-defined
- Protocol layer remains encoding-agnostic

---

## 10. Responses

### 10.1 Inline Response (legacy)

```
200 <data>
```

### 10.2 PAYLOAD Response

```
200 PAYLOAD <encoding> <length>
<raw bytes>
```

Rules:
- Same framing semantics as request payload
- Encoding is metadata only

After response:

```
SEND_RESPONSE → IDLE
```

---

## 11. Error Model

### Framing Errors (protocol engine)
- AGAIN
- OVERFLOW
- ERROR

### Semantic Errors (application)
- Returned as normal protocol responses (`400`, `500`, etc.)

---

## 12. Backward Compatibility

- Legacy CALL remains unchanged
- PAYLOAD is opt-in
- Encoding metadata does not affect legacy clients
- Old PHP encoding conventions continue to work

---

## 13. Rationale

- Keeps protocol minimal and robust
- Avoids PHP-specific leakage into core
- Enables efficient binary transport
- Supports multiple language bindings cleanly

---

## 14. Summary

This specification defines:

- **When PAYLOAD is allowed**
- **How payload bytes are framed**
- **How encoding is declared (but not interpreted)**

Encoding meaning is intentionally delegated to higher layers.

**This framing + metadata layer is intended to remain stable for the next decade.**
