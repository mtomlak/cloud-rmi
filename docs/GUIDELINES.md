# Cloud-RMI Development Guidelines

This document defines **non-negotiable rules** and **architectural intent**
for the Cloud-RMI project (`libcloudrmi`, C server, PHP client/server, proxy).

These rules exist to protect **10+ years of proven production behavior**
while allowing *careful, incremental evolution*.

They apply equally to:
- C core library (`libcloudrmi`)
- C server / proxy
- PHP client / server
- Future PHP extension
- Any automated agent (Codex, GPT, CI tooling)

---

## 1. Core Principles (READ FIRST)

1. **Backward compatibility is sacred**
   - Existing clients and servers must continue to work unchanged
   - Any behavior change must be opt-in (via capability or protocol version)

2. **Protocol stability over cleverness**
   - Prefer explicit framing over inference
   - Prefer state machines over heuristics
   - Never “guess” protocol intent from payload contents

3. **Separation of concerns is mandatory**
   - Transport ≠ Protocol ≠ Execution ≠ Language bindings
   - No layer may “peek” into another layer’s responsibilities

4. **Evolution, not rewrite**
   - This codebase is intentionally conservative
   - Improvements must integrate with existing structure, not replace it

---

## 2. Layer Responsibilities

### 2.1 libcloudrmi (Protocol Engine)

**Responsibilities:**
- Byte-level protocol parsing
- Framing (CALL, PAYLOAD, responses)
- State machine enforcement
- Capability & protocol version handling

**Must NOT:**
- Perform I/O (socket, pipe, file)
- Know about TCP, UNIX sockets, pipes, PHP, shell, or exec
- Allocate unbounded memory
- Execute calls or spawn processes

**Design rules:**
- Deterministic state machine
- Explicit error codes (no silent recovery)
- Zero ambiguity in framing
- Payload length is authoritative

---

### 2.2 C Server / Proxy

**Responsibilities:**
- Transport I/O (TCP, UNIX sockets, pipes)
- Forking / cloning / process lifecycle
- Authentication enforcement
- Dispatching validated protocol events
- Streaming payloads to/from handlers

**Must NOT:**
- Re-implement protocol parsing
- Interpret protocol semantics heuristically
- Modify payload framing rules
- Hide protocol errors

**Important:**
- Server MUST accept legacy protocol by default
- New behavior only after negotiated capability / version
- Proxy must preserve byte-exact semantics

---

### 2.3 PHP Client / Server (Userland)

**Responsibilities:**
- Provide ergonomic API
- Preserve historical behavior 1:1
- Decode / encode payloads correctly
- Respect negotiated capabilities

**Must NOT:**
- Invent protocol rules
- Assume server capabilities
- Change framing without confirmation
- Break existing application semantics

**Important:**
- PHP is a consumer of the protocol, not the owner
- PHP behavior follows protocol, never the opposite

---

### 2.4 PHP Extension (Future)

**Intent (not yet implemented):**
- Expose `libcloudrmi` APIs directly
- Allow zero-copy payload paths
- Enable fast IPC (UNIX sockets / pipes)
- Preserve protocol semantics exactly

**Non-goals:**
- Replacing the protocol
- PHP-specific protocol variants
- Hidden shared global state across requests

---

## 3. Protocol Evolution Rules

### 3.1 Versions vs Capabilities

- **Protocol versions** define framing rules
- **Capabilities** define optional features (stats, proxy, payload support)

Rules:
- Legacy clients default to legacy protocol
- New protocol is used only after explicit negotiation
- Capabilities MUST be advertised by the server
- Client behavior MUST follow negotiated result

---

### 3.2 Payload Rules (Authoritative)

1. Payload framing is explicit and length-delimited
2. Length is authoritative — never scan for terminators
3. Inline payloads are allowed only below agreed limits
4. Large payloads MUST use PAYLOAD framing
5. No implicit base64 fallback in protocol v2+

---

### 3.3 Responses

Allowed response forms (protocol v2+):

Inline (small payload):

```
200 <encoding> <len> <bytes>\n
```

Payload (large/binary):


```
200 PAYLOAD <encoding> <len>\n
<exactly len bytes>
```


Rules:
- Client MUST support both
- Server MAY choose
- Length mismatch is a protocol error

---

## 4. Performance & Safety Rules

- No unbounded reads
- No unbounded writes
- No silent truncation
- No environment-variable based behavior
- No global mutable protocol state
- All child processes must be reaped
- Errors must be explicit and recoverable

---

## 5. Testing Expectations

Any change that touches:
- protocol
- framing
- payload
- proxy
- child processes

**MUST include tests** covering:
- legacy behavior
- new behavior
- malformed input
- boundary conditions
- proxy passthrough

Regression prevention is mandatory.

---

## 6. What We Explicitly Avoid

- “Smart” protocol guessing
- Magic auto-detection
- Breaking changes hidden as refactors
- Language-specific protocol forks
- Over-engineering before real need

---

## 7. Final Rule

If a change:
- risks breaking old code
- complicates the protocol
- hides errors
- or cannot be clearly reasoned about

---
## 8. Next Steps
- Study `docs/PROTOCOL.md` **and** its companion `docs/PROTOCOL_IMPLEMENTATION.md`
  for protocol design and command semantics. Read them end-to-end before
  making any edits—skipping protocol details is considered a violation of
  these guidelines.


**It should not be merged.**

When in doubt: **do less, not more.**
