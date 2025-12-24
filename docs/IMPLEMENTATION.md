# Cloud-RMI — Implementation Notes

This document captures the **current implementation state** of the C library,
server, client, and proxy. It is **not** the protocol specification—see
`docs/PROTOCOL.md` for the normative protocol definition.

Different language implementations (C, PHP, etc.) may implement different
subsets of optional features. This document describes what the C implementation
currently does; other implementations may behave differently while remaining
protocol-compliant.

---

## 1. Parser Limits & State Machine

Source: `libcloudrmi/src/protocol_feed.c`, `libcloudrmi/include/cloudrmi/types.h`

- **Line length**: `MAX_LINE = RMI_BUF_SZ = 8192` bytes. Lines without a `\n`
  after 8192 bytes trigger `RMI_E_OVERFLOW` and reset the parser.
- **Payload ceiling**: `max_payload` is configurable per protocol context
  (default `RMI_DEFAULT_MAX_PAYLOAD = 10 MB`). Payload CALL headers with length
  above this ceiling are rejected and reported as overflow.
- **Inline payload soft limit**: `RMI_INLINE_PAYLOAD_THRESHOLD = RMI_MAX_LINE -
  floor(RMI_MAX_LINE * 0.35) - 500` (4,825 bytes with the current limits).
  Clients prefer payload framing when raw arguments exceed this threshold; for
  smaller payloads they still verify the fully framed, base64-encoded CALL line
  does not reach `RMI_MAX_LINE`, falling back to payload framing when the
  encoded line is too large.
- **Payload buffer strategy**: Each protocol context maintains a reusable
  payload buffer. By default it preallocates 128 KB
  (`RMI_PAYLOAD_PREALLOC_ENABLED` / `RMI_PAYLOAD_PREALLOC`) and grows via
  `realloc()` when payloads are larger, freeing lazily on context destroy.
  Compile-time flags can disable preallocation for RAM-conservative builds.
- **States**: `IDLE → READING_PAYLOAD → EXECUTE_CALL → SEND_RESPONSE`. Each
  request begins with a single control line; payload framing metadata lives on
  the CALL line in protocol v2.
- **Protocol versions**: validated `1..2`. Invalid `PROTO` lines reset the
  parser and surface `RMI_ERROR_BAD_REQUEST`.
- **PAYLOAD handling**:
  - Zero-length payloads are legal. The parser emits a CALL event immediately
    with an empty payload buffer.
  - Payload CALL headers take the form `CALL <iface> <method> PAYLOAD <encoding>
    <length>` (v2 only). Length must be decimal and ≤ `max_payload`;
    otherwise the parser resets and reports `RMI_ERROR_BAD_REQUEST` (or
    `RMI_E_OVERFLOW` when above the ceiling).
  - Readers MUST consume exactly the declared payload length. Responses do not
    include a trailing delimiter byte, and any speculative read beyond
    `<length>` risks consuming the first byte of the next header.
- **Error propagation**: `RMI_ERROR_BAD_ARGS` is used for missing CALL tokens or
  malformed payload headers; `RMI_ERROR_BAD_REQUEST` covers malformed versions,
  or unexpected command ordering.

---

## 2. Capability Advertising

Sources: `src/protocol.c`, `src/capabilities.c`

- Banner format: `220 <context> Server Ready EXT PROTO=2[,<caps>]`.
- Capabilities advertise **optional protocol extensions**, not core commands.
- Core commands (CALL, PING, AUTH, HELP, etc.) are part of the base protocol
  and are NOT advertised as capabilities.
- C implementation capabilities:
  - `SERVER` — the SERVER command namespace is implemented.
  - `PROXY` — proxy mode is compiled in (only when built with `PROXY_SUPPORT`).
- Other implementations may advertise different capabilities (e.g., PHP servers
  advertise `ACALL` for async call support).
- Clients and servers MUST ignore unknown capability tokens gracefully—this
  allows implementations to evolve independently.
- Tokens are comma-separated in the banner, then normalized to spaces by
  clients before parsing.

---

## 3. Server Command Handling & Status Codes

Source: `src/protocol.c`

- All command responses are single-line; JSON replies use compact formatting
  with the explicit `JSON` marker, and binary data is always returned via the
  PAYLOAD channel.
- **AUTH**
  - `AUTH TOKEN <value>` → `200 Hello <user>` when `value == var_recovery_token`,
    otherwise `401 Unauthorized`.
  - `AUTH dbg` is a development backdoor → `200 Hello Developer`.
  - Re-auth attempts respond with `200 Already logged in`.
  - Unknown AUTH mode → `400 Bad Request Arguments`.
- **PROTO**
  - Accepts versions 1–2. Outside this range → `400 Bad Request`.
  - Successful negotiation sets connection state and returns `200 OK`.
- **CALL**
  - Requires authentication; otherwise `401 Unauthorized`.
  - Inline CALL with `< 3` args → `400 Bad Request Arguments`.
  - Dispatches to the proxy (when enabled) or local handler.
- **HELP**: `200 JSON ["AUTH", ...]` listing supported commands (conditionally
  includes `PROXY`). Single-line only.
- **IFACES**: `200 JSON ["iface1","iface2"]` from proxy or local server map.
- **TIME**: RFC822 timestamp → `200 <time>`.
- **PING**: `200 PONG`. Optional echo argument: `PING hello` → `200 PONG hello`.
- **CAPS**: `200 JSON ["SERVER","PROXY"]` derived from compiled capabilities.
- **INFO**: `200 JSON {...}` returning structured node information including
  role (server/proxy), capabilities, build info (language, kernel, version),
  and runtime info (pid, uptime). Read-only and always allowed.
- **QUIT/EXIT**: `221 Bye` and closes connection.
- **PROXY stats**: `proxy stats` → `200 JSON {"servers":[...],"clients":[...],"trimmed":false}`;
  other args → `400 Bad Request Arguments` (compiled only when
  `PROXY_SUPPORT` is enabled). Output is capped to avoid overflowing the
  single response line.
- **Unknown commands**: `400 Bad Request`.

---

## 3a. SERVER Extension (C Implementation)

The SERVER command namespace provides implementation-specific subcommands.
Availability and behavior of subcommands varies by implementation—what works
in C may not work in PHP and vice versa. Clients should use `SERVER HELP` to
discover available subcommands.

**C server subcommands:**

- `SERVER HELP` → `200 JSON ["DEBUG","RELOAD","SHUTDOWN","WORKERS"]`
  listing available subcommands for this implementation.
- `SERVER DEBUG on/off` (auth-only) toggles debug logging. `SERVER DEBUG`
  without argument returns current state.
- `SERVER DEBUG TRAFFIC on/off` (auth-only) toggles traffic logging.
- `SERVER RELOAD` (auth-only) reloads configuration and interface map.
- `SERVER SHUTDOWN` → `404 Unsupported` in C implementation.
- `SERVER WORKERS` → `200 JSON {...}` single-line JSON with worker/process
  details (capped to 25 entries), `trimmed` flag when truncated.
  Only available when worker reuse is compiled in.

**PHP server subcommands (may differ):**

- `SERVER SHUTDOWN` → actually shuts down the server (PHP supports this).
- `SERVER RELOAD` → `200 Reload not implemented` (PHP does not support hot reload).
- `SERVER DEBUG` → returns current state only (PHP has no runtime debug toggle).

Implementations should return appropriate status codes for unsupported
subcommands (e.g., `404 Unsupported` or `400 Bad Request Arguments`).

Parser-level errors map to responses inside `handle_connection`:
- `RMI_E_OVERFLOW`/`RMI_E_ERR` from `rmi_protocol_feed` → `400 Bad Request`.
- `RMI_ERROR_BAD_REQUEST` → `400 Bad Request`.
- `RMI_ERROR_BAD_ARGS` → `400 Bad Request Arguments`.

---

## 4. CALL Execution Pipeline

Sources: `src/handle_call.c`, `src/server.c`

- Authentication is enforced before dispatch; unauthorized callers receive
  `401 Unauthorized` and no handler execution.
- Interface resolution uses the configured map (`server_get_iface_entry`).
  Missing interfaces yield `400 Unsupported interface: <iface>`.
- **Inline CALL (v1/v2)**: Arguments are base64-decoded and split with
  POSIX-style shell quoting rules into `argv[2+]`. Empty quoted tokens are
  preserved.
- **PAYLOAD CALL (v2)**:
  - Handler stdin receives the exact payload bytes. Zero-length payloads close
    stdin immediately.
  - `argv[2]` carries the encoding token (empty string allowed); no synthetic
    argument is appended for payload calls.
- **Process management**:
  - Handler executed via `fork()` + `execv(file, argv)`.
  - Stdout/stderr are multiplexed back to the client; polling keeps stdout and
    stdin streaming without blocking.
  - Trailing `\n`/`\r` are stripped from handler output before framing.
- **Response framing**:
  - If payload was provided **and** client negotiated v2: `200 PAYLOAD
    <encoding> <len>` followed by raw bytes. Encoding defaults to request
    encoding or `PLAIN` when empty.
  - Otherwise: base64-encodes stdout and responds `200 <base64>` (no encoding
    token prefix).
- **Error responses**:
  - Output beginning with `"Error"` → `500 Exited with error message: <output>`.
  - Non-zero exit codes → `500 Error: <output> (<code>)`.
  - Setup failures (pipes, forks, decoding) use explicit `500 Error ...`
    messages.

---

## 5. Client Behavior

Source: `src/client.c`

- Banner parsing extracts `PROTO=<n>` and advertised capabilities. Capability
  commas are normalized to spaces before `rmi_parse_capabilities`.
- If the banner omits capabilities, the client issues `CAPS` to discover them.
- When the server advertises `PROTO >= 2`, the client attempts `PROTO 2` and
  updates `conn->protocol_version` on `200 OK`.
- **PAYLOAD gating**:
  - Requires negotiated PROTO 2. Otherwise the call
    aborts locally with status `505` and an explanatory message.
- **CALL PAYLOAD flow**: Sends `CALL iface method PAYLOAD <encoding> <len>`,
  writes the payload bytes, and parses the first response line.
- **Multiline responses**: Any response ending with `:` triggers an additional
  `stream_readlns` read to collect trailing lines.
- **Decoding helpers**: Inline responses may include `encoding:base64`. The
  client strips the prefix when present before base64-decoding.

---

## 6. Proxy Bridging

Source: `src/proxy.c`

- Maintains a pool of upstream connections per proxy target. Spare connections
  are pre-created (`var_proxy_min_spare_conns`) and kept alive via periodic
  `PING`.
- Upstream connections reuse the standard client flow: connect, optional token
  authentication, banner parsing, capability discovery, and PROTO negotiation.
- **CALL forwarding**:
  - Payload requests: send unified `CALL <iface> <method> PAYLOAD <encoding>
    <len>` headers then stream payload bytes.
  - Inline requests: send legacy `CALL iface method <base64-args>`.
- **Response adaptation**:
  - Payload responses (`status 200/500` with `PAYLOAD <encoding> <len>`) are
    streamed directly when the downstream client is v2. For v1 clients, the
    proxy base64-encodes the payload, preserves the encoding token, and
    responds inline (`<status> <encoding>:<base64>`).
  - Inline responses are passed through unchanged.
- **Resilience**:
  - Missing/timeout responses (status `0` or `408`) trigger a reconnect and
    retry once. Persistent failure yields `503 Service Unavailable` to the
    client.
  - Interface discovery expects `200 JSON ["iface", ...]` from upstream
    servers (legacy multi-line IFACES output is not supported). Connection
    failures during discovery or dispatch trigger a 60-second cooldown before
    the next reconnect attempt.
  - Proxy stats are available via `proxy stats` and list connection/worker
    state.

---

## 7. Error Mapping & Recovery Expectations

- **Library statuses** (`rmi_status_t`):
  - `RMI_OK` — event ready.
  - `RMI_E_AGAIN` — need more bytes; no response is emitted yet.
  - `RMI_E_OVERFLOW` — line or buffer limit exceeded; parser resets.
  - `RMI_E_ERR` — malformed input or invalid sequencing; parser resets.
- **Error codes** (`rmi_error_code_t`):
  - `RMI_ERROR_BAD_REQUEST` — malformed command, invalid version/length,
    unexpected payload framing, buffer failures.
  - `RMI_ERROR_BAD_ARGS` — missing required CALL or payload parameters.
- **Server responses**:
  - `400 Bad Request` on BAD_REQUEST or transport overflow.
  - `400 Bad Request Arguments` on BAD_ARGS.
  - `401 Unauthorized` for unauthenticated CALL or auth-only SERVER subcommands.
  - `404 Unsupported` for unimplemented optional commands.
  - `500 ...` for handler or pipeline failures.
- **Client-local errors**: Lack of negotiated payload support returns status
  `505` with a human-readable explanation; no network request is sent.

---

## 8. Operational Notes

- Payload and capability negotiation are deliberately explicit; no auto-upgrade
  occurs even when the server advertises higher versions.
- Proxy and server banners separate capability tokens with commas, but all
  downstream parsing normalizes to spaces—ensure documentation and tests use
  the normalized form when asserting capabilities.
- Handler stdout is trimmed of trailing newlines before framing; binary payload
  responses preserve raw bytes without trimming.
- JSON emission for HELP/IFACES/CAPS/debug/proxy stats is intentionally
  hand-crafted (single-line, compact) to avoid heavyweight dependencies; keep
  the marker `200 JSON` and the JSON string as the final response token.
- Unix domain socket listeners can opt into trusted transport with
  `unix_socket_skip_auth = On`; when enabled the server marks the connection as
  authenticated immediately without changing protocol semantics.
- Unix socket transports always run in protocol v2 state and reject protocol
  downgrades below v2; TCP listeners retain the legacy default of v1 unless
  explicitly upgraded.
