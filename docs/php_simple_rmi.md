# PHP Simple RMI classes

The PHP reference client/server pair (`SimpleRMIClient`, `SimpleRMIServer`) now mirrors the C reference implementation and the `libcloudrmi` parser semantics while preserving the legacy PHP API.

## Negotiation and capabilities

- Connections always start on protocol v1 and upgrade only after an explicit `PROTO 2` exchange.
- The server banner follows the canonical format `220 <context> Server Ready EXT PROTO=<max>[,<capabilities>]`.
- `CAPS` is supported to fetch a normalized capability list when the banner omits it.
- Payload framing is gated by two conditions: the negotiated protocol version must be `2` **and** the `PAYLOAD` capability must be advertised. The client will fall back to inline calls when either gate is missing.

## Payload handling

- Inline calls remain compatible with the legacy v1 framing and small payload policy.
- Payload calls use the v2 framing (`CALL iface method <encoding> <length>` + raw bytes) and respect the `RMI_MAX_BUFFER` ceiling when defined.
- Responses for payload calls reuse the requested encoding when possible and fall back to `PLAIN`/`RAW` for string data or `PHP` for structured results.
- The deprecated `BLOB` encoding is removed; binary transfers now rely on the v2 payload channel.

## ACALL extension

- Asynchronous execution is exposed as the vendor capability `ACALL` and advertised in both the banner and `CAPS` output when enabled.
- `SimpleRMIServer` only advertises and honors `ACALL` when forking support is available **and** `$enableAsyncCapability` is `true`. Set this property to `false` in subclasses to disable the extension.
- The client issues `ACALL` commands only when the capability is advertised; otherwise it transparently falls back to synchronous `CALL` semantics even when `async` was requested.
- ACALL responses acknowledge with `200 Call Executed`; work is executed in a forked child and does not stream a result back to the caller.

## Configuration knobs

- `$enablePayloadCapability` controls whether the server advertises and accepts v2 payload calls.
- `$enableAsyncCapability` controls the `ACALL` capability as described above.
- `setMaxProtocolVersion()` clamps to the supported range (v1–v2) and drives banner negotiation.

These behaviors are covered by the PHP integration tests under `php/tests/integration`, including payload negotiation, legacy clients, and ACALL capability coverage.
