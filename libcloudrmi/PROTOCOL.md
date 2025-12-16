# Cloud-RMI Protocol v2 – Framing, Negotiation, and Proxy Semantics

## 1. Line Length & Framing Limits

### Hard limits
- MAX_LINE = 8192 bytes  
- Any command line must contain '\n' within 8191 bytes  
- Otherwise: 400 Bad Request

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
