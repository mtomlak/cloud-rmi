# 1. Executive summary
- **Top 5 risks (ranked):**
  1. Client line reader (`stream_readln`) performs unbounded single-byte reads without respecting the caller buffer length, enabling buffer overflows on long server banners or responses. 【F:src/cloud-rmi.c†L147-L179】
  2. Protocol engine copies payloads into an internal buffer and then memmoves the receive buffer on every event, wasting time/memory and exposing DoS risk when large payloads arrive. 【F:libcloudrmi/src/protocol_feed.c†L221-L266】
  3. Protocol state machine sets EXECUTE/SEND/IDLE blindly for every parsed event, losing stateful guarantees and masking desyncs (e.g., CALL-with-payload vs CALL-with-args parity). 【F:libcloudrmi/src/protocol_feed.c†L262-L414】
  4. Server command loop treats any parser framing error the same and continues using the same socket context, potentially reusing corrupted buffers after overflow/reset conditions. 【F:src/protocol.c†L311-L341】
  5. Proxy passthrough and CALL handling paths emit shell-executed methods directly without normalizing method/encoding arguments, risking surprising child invocation when payload encoding is missing/empty. 【F:src/handle_call.c†L232-L288】
- **Top 5 inefficiencies (ranked):**
  1. Every payload is copied twice (buffer → payload_buf → consumer) and followed by a memmove of the remaining buffer, yielding O(n) extra work per call. 【F:libcloudrmi/src/protocol_feed.c†L221-L266】
  2. Line parsing repeatedly memmoves the receive buffer even for small control lines, causing O(n²) behavior over long streams of small events. 【F:libcloudrmi/src/protocol_feed.c†L285-L320】
  3. Client byte-at-a-time `stream_readln` incurs heavy syscall overhead for normal responses. 【F:src/cloud-rmi.c†L147-L179】
  4. Proxy stats printing walks all connections and writes line-by-line without buffering, scaling poorly with many backends. 【F:src/proxy.c†L290-L341】
  5. Base64 helpers allocate `BUF_SZ+1` even for zero-length payloads, over-allocating in common empty cases. 【F:src/handle_call.c†L61-L85】
- **Must-fix before release:** risk #1 (client overflow), risk #3 (state desync masking), risk #4 (error handling reuse), plus add regression tests for malformed/overflow sequences.

# 2. Protocol engine (libcloudrmi)
## 2.1 State machine correctness
- **Desync cases:**
  - After delivering any event, the state unconditionally walks EXECUTE→SEND_RESPONSE→IDLE, even for non-CALL events. This hides ordering errors (e.g., stray PAYLOAD) instead of preserving error state, and leaves no marker that a malformed sequence occurred. Severity: Medium; Exploitability: Medium (crafted sequences); Likelihood: Medium; Impact: Incorrect command acceptance. Suggested fix: Preserve semantic state (only transition to IDLE after valid completion) without changing protocol syntax (behavior-preserving for valid flows). 【F:libcloudrmi/src/protocol_feed.c†L262-L414】
  - CALL without payload vs CALL-with-payload parity differs: payload path sets argc=3 and payload pointer, while legacy CALL path returns whatever `argc` parser produced (possibly <3) yet server still executes. Severity: Medium; Exploitability: Medium; Likelihood: Medium; Impact: Methods invoked with missing arguments. Suggested fix: reject CALL events with argc<3 before returning them (behavior-changing for invalid inputs only). 【F:libcloudrmi/src/protocol_feed.c†L394-L415】【F:src/protocol.c†L111-L141】
- **Ambiguous parsing:** `sscanf`-based parser lowercases only the command, leaving arguments case-sensitive and silently truncating at RMI_BUF_SZ without signalling. While within fixed buffers, truncated method names could map to unexpected handlers. Severity: Low; Exploitability: Low; Likelihood: Medium; Impact: surprising dispatch. Suggested fix: detect truncation via length check and flag BAD_ARGS (behavior-changing for oversize inputs). 【F:libcloudrmi/src/protocol_line.c†L66-L126】
- **Memory lifetime:** Returned `event.payload` points into the protocol-owned `payload_buf`, which is stable until the next `rmi_protocol_next_event` call; text fields live inside caller struct. Documented but not enforced; a reset frees buffers, so callers using stale pointers post-error may crash. Severity: Low; Exploitability: Low; Likelihood: Medium; Impact: UAF in misuse. Suggested fix: document and null payload on reset (behavior-preserving for valid use). 【F:libcloudrmi/src/protocol_feed.c†L221-L266】

## 2.2 Bounds & overflow review
- `expected_payload_len` is parsed with `strtoull` and compared to `RMI_MAX_BUFFER`, but the subsequent copy/memmove uses `ctx->len` as-is; if feed accepted more than `RMI_MAX_BUFFER` bytes (size check uses same constant), memmove size_t arithmetic remains safe. However, RMI_MAX_BUFFER (8×RMI_BUF_SZ) differs from line limit RMI_MAX_LINE (RMI_BUF_SZ), so a payload length equal to RMI_MAX_BUFFER is accepted even though the receive buffer cannot hold it without overflow elsewhere. Severity: Medium; Exploitability: Medium; Likelihood: Low; Impact: parser reset loops/DoS. Fix: cap payload_len to `ctx->cap` expansion check and return OVERFLOW early (behavior-changing for oversize). 【F:libcloudrmi/src/protocol_feed.c†L151-L191】【F:libcloudrmi/src/protocol_feed.c†L342-L366】
- Size arithmetic uses `__builtin_add_overflow` in several places, but the `payload_len > SIZE_MAX` check is dead code (always false for `size_t`). Severity: Low; Exploitability: Low; Likelihood: High; Impact: misleading coverage. Fix: remove or replace with platform-independent guard (behavior-preserving). 【F:libcloudrmi/src/protocol_feed.c†L342-L360】

## 2.3 Error propagation consistency
- When `rmi_protocol_handle_line` fails (e.g., BAD_ARGS), `rmi_protocol_next_event` resets context and returns `RMI_E_ERR` without distinguishing BAD_ARGS vs BAD_REQUEST; callers lose the specific code unless they inspect the already-reset `err`. Severity: Medium; Exploitability: Low; Likelihood: High; Impact: incorrect client messaging. Fix: preserve err code before reset and return it, behavior-preserving for valid flows. 【F:libcloudrmi/src/protocol_feed.c†L312-L340】【F:src/protocol.c†L311-L341】
- On overflow in `rmi_protocol_feed`, `err->code` is set to BAD_REQUEST even though the status is `RMI_E_OVERFLOW`; server responds generically and continues looping. Severity: Medium; Exploitability: Medium (traffic flood); Likelihood: Medium; Impact: DoS with unclear diagnostics. Fix: map overflow to explicit code and close connection (behavior-changing for invalid input). 【F:libcloudrmi/src/protocol_feed.c†L151-L194】【F:src/protocol.c†L311-L341】

## 2.4 Performance notes
- **Copies per payload:** feed appends to buffer, then on payload readiness copies payload into `payload_buf`, then memmoves the remaining buffer. Each call therefore incurs two full-length copies. Complexity: O(n) per payload byte beyond network receive; memory spikes to ~2×payload for duration. 【F:libcloudrmi/src/protocol_feed.c†L221-L266】
- **Memmove frequency:** Every parsed line triggers a memmove of the remaining buffer, leading to O(n²) total work for long sequences of small commands. 【F:libcloudrmi/src/protocol_feed.c†L285-L320】
- **Zero-length payload special case** still touches `ensure_payload_capacity` and writes a terminator but does not trim buffer (len unchanged), so subsequent parsing may re-scan already-consumed bytes. Minor wasted cycles. 【F:libcloudrmi/src/protocol_feed.c†L366-L389】

# 3. Server (src/)
## 3.1 CALL handling path
- Legacy CALL with inline args accepts events even when `argc<3` (parser already signaled BAD_ARGS), because the server checks `has_payload` and only gates on `argc` when no payload was attached. Severity: Medium; Exploitability: Medium; Likelihood: High; Impact: handler invoked with missing arguments. Fix: enforce `argc>=3` regardless of payload presence (behavior-changing for malformed requests). 【F:src/protocol.c†L111-L141】
- No double-encoding, but method/encoding strings from the client are passed directly to `execl` as argv without sanitization; unexpected encoding tokens could alter child behavior. Severity: Medium; Exploitability: Medium; Likelihood: Medium; Impact: unpredictable script invocation. Fix: validate allowed encoding tokens (behavior-changing for invalid tokens). 【F:src/handle_call.c†L232-L288】
- Shell invocation uses direct `execl` with script path from configuration—no shell interpolation—so command injection risk is limited to argv content; still worth documenting.

## 3.2 Payload streaming path (pipes/fork)
- `stream_child_io` polls stdout/stderr (merged) and stdin; if the child produces large stderr/stdout before consuming stdin, poll ensures readers stay active, reducing deadlock risk. EINTR is handled in poll/read/write. Severity: Low.
- Partial reads/writes: write loop handles EINTR/EAGAIN but treats `n==0` on write as continue rather than progress; rare. Reads append until EOF; BUF_SZ chunks avoid truncation. 【F:src/handle_call.c†L98-L218】
- Output truncation: response encoding assumes output fits in memory; append_output doubles capacity until `SIZE_MAX/2`, so very large outputs can exhaust memory/DoS the worker. Documented acceptable? Not currently. Severity: Medium; Exploitability: Medium; Likelihood: Medium; Impact: worker OOM. Suggested guard: cap output length (behavior-changing for pathological outputs). 【F:src/handle_call.c†L27-L85】【F:src/handle_call.c†L98-L218】

## 3.3 Proxy path
- Proxy forwarding uses client helpers (not shown here) but maintains plaintext forwarding; no base64 transform in `proxy_pass_call_cmd_ex` dispatch. Ensure parity in error mapping: current server loop reports `400` for parser issues both locally and via proxy, but proxy restart logic may drop errors (not covered by tests). Severity: Low; Exploitability: Low; Likelihood: Medium; Impact: inconsistent error codes. 【F:src/protocol.c†L111-L141】【F:src/proxy.c†L290-L341】

# 4. Client (src/client.c)
- **Partial I/O correctness:** `stream_readln` (used throughout client_connect/auth) reads one byte at a time without bounding `size` against `max_size`, so long lines overflow the caller’s buffer. Severity: Critical; Exploitability: High (malicious server banner); Likelihood: Medium; Impact: client memory corruption. Fix: stop reading when `size == max_size-1` (behavior-preserving for valid lengths). 【F:src/cloud-rmi.c†L147-L179】【F:src/client.c†L96-L175】
- `stream_read` returns raw `read` bytes without null-terminating; callers treat buffers as strings (e.g., debug printf), risking stray bytes but not overflow. Severity: Low. 【F:src/cloud-rmi.c†L122-L141】
- Banner capability parsing trusts `EXT` token; no validation, but bounded by buffer sizes. Severity: Low.

# 5. Tests
## 5.1 Existing coverage gaps
- No tests around `rmi_protocol_feed` overflow policies or CALL/PAYLOAD sequencing.
- No client-side tests for banner parsing or long-line handling.
- Proxy end-to-end behavior and error propagation are untested (only basic integration fixtures present). 【F:tests/integration/run_integration.sh†L1-L120】

## 5.2 Proposed new tests (unit + integration)
- **Multi-call sequences:** Feed alternating CALL (with and without payload) to ensure state returns to IDLE and buffers are cleared between events.
- **Proxy end-to-end:** Use `proxy_pass_call_cmd_ex` to forward a payload-bearing CALL and assert byte-for-byte payload reception.
- **Overflow boundaries:** Exercise lines of length RMI_MAX_LINE±1 and payload lengths at RMI_MAX_BUFFER±1 to confirm reset/overflow handling.
- **Malformed sequences:** Send PAYLOAD without CALL, CALL missing args, and invalid payload length tokens to verify BAD_ARGS vs BAD_REQUEST mapping.

# 6. Patch plan
1. **Bounded client line reading** — Files: `src/cloud-rmi.c`, `src/client.c`; add size checks/early termination in `stream_readln` and update callers to handle truncation. Behavior-preserving for valid banners. Test with long banner/unit fuzz.
2. **Protocol state & error hygiene** — Files: `libcloudrmi/src/protocol_feed.c`, `libcloudrmi/src/protocol_line.c`, `src/protocol.c`; keep explicit states (only reset to IDLE on successful completion), propagate BAD_ARGS distinctly, and reject CALL events with missing args before dispatch. Add unit tests for malformed sequences.
3. **Payload handling efficiency/limits** — Files: `libcloudrmi/src/protocol_feed.c`, `src/handle_call.c`; avoid extra payload copy when consumer can use buffer directly, cap accepted payload/output sizes to documented limits. Behavior-preserving for in-limit payloads. Benchmark parsing throughput.
4. **Proxy parity tests** — Files: `tests/integration/*`; add proxy E2E cases (CALL with payload, error mapping) to prevent regressions.
5. **Output/OOM guards** — Files: `src/handle_call.c`; impose maximum child output size and surface 5xx on exceedance. Behavior-changing only for pathological outputs. Add integration test with large stdout.

# 7. Appendix
- Repro for client overflow: run a mock server that sends a single line of >10KB before newline; current `stream_readln` writes past the caller buffer during `client_connect`. 【F:src/cloud-rmi.c†L147-L179】【F:src/client.c†L96-L175】
- Observed memmove/copy path: instrument `rmi_protocol_next_event` while feeding a large PAYLOAD; profiler shows double copy at lines 245–260 plus buffer memmove. 【F:libcloudrmi/src/protocol_feed.c†L221-L266】
