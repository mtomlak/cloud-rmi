# Cloud-RMI Protocol — Implementation Companion

This document captures the **code-level semantics** enforced by the C library,
server, client, and proxy. It expands on `docs/PROTOCOL.md` with precise limits,
status codes, and bridging behaviors.

---

## 1. Parser Limits & State Machine

Source: `libcloudrmi/src/protocol_feed.c`, `libcloudrmi/include/cloudrmi/types.h`

- **Line length**: `MAX_LINE = RMI_BUF_SZ = 8192` bytes. Lines without a `\n`
  after 8192 bytes trigger `RMI_E_OVERFLOW` and reset the parser.
- **Total buffer ceiling**: `RMI_MAX_BUFFER = RMI_BUF_SZ * 8` (65536 bytes).
  Exceeding this resets the parser and reports `RMI_E_OVERFLOW` with
  `RMI_ERROR_BAD_REQUEST`.
- **Inline payload soft limit**: `RMI_INLINE_PAYLOAD_THRESHOLD = RMI_MAX_LINE -
  floor(RMI_MAX_LINE * 0.35) - 500` (4,825 bytes with the current limits).
  Clients prefer payload framing when raw arguments exceed this threshold; for
  smaller payloads they still verify the fully framed, base64-encoded CALL line
  does not reach `RMI_MAX_LINE`, falling back to payload framing when the
  encoded line is too large.
- **States**: `IDLE → CALL_PENDING_PAYLOAD → READING_PAYLOAD →
  EXECUTE_CALL → SEND_RESPONSE`. Pending CALL state is used when a payload
  header is received without inline args.
- **Protocol versions**: validated `1..2`. Invalid `PROTO` lines reset the
  parser and surface `RMI_ERROR_BAD_REQUEST`.
- **PAYLOAD handling**:
  - Zero-length payloads are legal. The parser emits a CALL event immediately
    with an empty payload buffer.
  - Payload length must be decimal and ≤ `RMI_MAX_BUFFER`; otherwise the parser
    resets and reports `RMI_ERROR_BAD_REQUEST` (or `RMI_E_OVERFLOW` when above
    the ceiling).
- **Error propagation**: `RMI_ERROR_BAD_ARGS` is used for missing CALL/PAYLOAD
  tokens; `RMI_ERROR_BAD_REQUEST` covers malformed versions, payload lengths,
  or unexpected command ordering.

---

## 2. Capability Advertising

Sources: `src/protocol.c`, `src/capabilities.c`

- Banner format: `220 <context> Server Ready EXT PROTO=2[,<caps>]`.
- Built-in capability tokens: `PAYLOAD`, `PROXY`, `STATS`.
- Compiled set: `PAYLOAD` and `STATS` are always emitted; `PROXY` is present
  only when built with `PROXY_SUPPORT`.
- Tokens are comma-separated in the banner, then normalized to spaces by
  clients before parsing.

---

## 3. Server Command Handling & Status Codes

Source: `src/protocol.c`

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
- **HELP**: Lists supported commands (conditionally includes `PROXY`).
- **IFACES**: Returns interface list from proxy or local server map.
- **RELOAD**: Auth-only. Reloads configuration and interface map → `200`.
- **SHUTDOWN**: Always returns `404 Unsupported`.
- **TIME**: RFC822 timestamp → `200 <time>`.
- **UPTIME**: Auth-only. Returns formatted uptime string → `200 ...` or
  `401 Unauthorized` when unauthenticated.
- **NOOP**: `200 OK`.
- **CAPS**: `200 <tokens>` derived from compiled capabilities.
- **DEBUG** (auth-only):
  - `debug on/off` toggles `var_debug`.
  - Otherwise prints build/runtime info (language, kernel API, proxy support,
    current PID, config paths).
- **QUIT/EXIT**: `221 Bye` and closes connection.
- **PROXY stats**: `proxy stats` → `200 Proxy is <ON|OFF> ...`; other args →
  `400 Bad Request Arguments` (compiled only when `PROXY_SUPPORT` is enabled).
- **SERVER stats**: `server stats` → `200` with worker list; other args → 400.
- **Unknown commands**: `400 Bad Request`.

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
- **Inline CALL (v1/v2)**: Arguments are base64-decoded before invocation.
- **PAYLOAD CALL (v2)**:
  - Handler stdin receives the exact payload bytes. Zero-length payloads close
    stdin immediately.
  - `argv[2]` carries the encoding token (empty string allowed); `argv[3]` is
    `"-"` for payload calls.
- **Process management**:
  - Handler executed via `fork()` + `execl(file, file, method, ...)`.
  - Stdout/stderr are multiplexed back to the client; polling keeps stdout and
    stdin streaming without blocking.
  - Trailing `\n`/`\r` are stripped from handler output before framing.
- **Response framing**:
  - If payload was provided **and** client negotiated v2: `200 PAYLOAD
    <encoding> <len>` followed by raw bytes and a terminal newline. Encoding
    defaults to request encoding or `PLAIN` when empty.
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
  - Requires negotiated PROTO 2 *and* `PAYLOAD` capability. Otherwise the call
    aborts locally with status `505` and an explanatory message.
- **CALL PAYLOAD flow**: Sends `CALL iface method`, then `PAYLOAD <encoding>
  <len>`, writes the payload bytes, and parses the first response line.
- **Multiline responses**: Any response ending with `:` triggers an additional
  `stream_readlns` read to collect trailing lines.
- **Decoding helpers**: Inline responses may include `encoding:base64`. The
  client strips the prefix when present before base64-decoding.

---

## 6. Proxy Bridging

Source: `src/proxy.c`

- Maintains a pool of upstream connections per proxy target. Spare connections
  are pre-created (`var_proxy_min_spare_conns`) and kept alive via periodic
  `NOOP`.
- Upstream connections reuse the standard client flow: connect, optional token
  authentication, banner parsing, capability discovery, and PROTO negotiation.
- **CALL forwarding**:
  - Payload requests: send `CALL` + `PAYLOAD <encoding> <len>` then stream
    payload bytes.
  - Inline requests: send legacy `CALL iface method <base64-args>`.
- **Response adaptation**:
  - Payload responses (`status 200/500` with `PAYLOAD <encoding> <len>`) are
    streamed directly when the downstream client is v2. For v1 clients, the
    proxy base64-encodes the payload and responds inline (`<status>
    <base64>`).
  - Inline responses are passed through unchanged.
- **Resilience**:
  - Missing/timeout responses (status `0` or `408`) trigger a reconnect and
    retry once. Persistent failure yields `503 Service Unavailable` to the
    client.
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
    unexpected PAYLOAD, buffer failures.
  - `RMI_ERROR_BAD_ARGS` — missing required CALL/PAYLOAD parameters.
- **Server responses**:
  - `400 Bad Request` on BAD_REQUEST or transport overflow.
  - `400 Bad Request Arguments` on BAD_ARGS.
  - `401 Unauthorized` for unauthenticated CALL/UPTIME/RELOAD/DEBUG.
  - `404 Unsupported` for `SHUTDOWN`.
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
