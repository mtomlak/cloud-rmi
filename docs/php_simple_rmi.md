# PHP Cloud RMI classes

The PHP reference client/server pair (`ProfiWeb\CloudRMI\CloudRMIClient`, `ProfiWeb\CloudRMI\CloudRMIServer`) is shipped as the Composer package `profiweb/cloudrmi` under `php/cloudrmi`. It mirrors the C reference implementation and the `libcloudrmi` parser semantics while preserving the legacy PHP API under the new namespace. The legacy `SimpleRMI*` aliases have been removed.

## Negotiation and capabilities

- TCP connections always start on protocol v1 and upgrade only after an explicit `PROTO 2` exchange. Unix socket transports require protocol v2, refuse downgrades, and are created via `setUnixSocketPath()` on the server and the `$unixSocketPath` constructor argument on the client.
- The server banner follows the canonical format `220 <context> Server Ready EXT PROTO=<max>[,<capabilities>]`.
- `CAPS` is supported to fetch a normalized capability list when the banner omits it.
- Payload framing is gated by protocol negotiation. The server always advertises `PAYLOAD` when its maximum protocol version is at least `2`, and payload framing is accepted only after a `PROTO 2` upgrade. The client falls back to inline calls when protocol v2 is unavailable.
- Trusted unix socket listeners can skip authentication by calling `setUnixSocketSkipAuth(true)`, mirroring the `unix_socket_skip_auth` behavior in the C server.
- The PHP client keeps a **defensive 256 KB cap** for inline protocol v1 responses; larger replies require payload framing (protocol v2).

## Payload handling

- Inline calls remain compatible with the legacy v1 framing and small payload policy.
- Payload calls use the unified v2 framing (`CALL iface method PAYLOAD <encoding> <length>` + raw bytes) and respect the configurable `RMI_MAX_PAYLOAD` ceiling (default 10 MB) on both client and server.
- Responses for payload calls reuse the requested encoding when possible and fall back to `PLAIN`/`RAW` for string data or `PHP` for structured results.
- The deprecated `BLOB` encoding is removed; binary transfers now rely on the v2 payload channel.

## ACALL extension

- Asynchronous execution is exposed as the vendor capability `ACALL` and advertised in both the banner and `CAPS` output when enabled.
- `CloudRMIServer` only advertises and honors `ACALL` when forking support is available **and** `$enableAsyncCapability` is `true`. Set this property to `false` in subclasses to disable the extension.
- The client issues `ACALL` commands only when the capability is advertised; otherwise it transparently falls back to synchronous `CALL` semantics even when `async` was requested.
- ACALL responses acknowledge with `200 Call Executed`; work is executed in a forked child and does not stream a result back to the caller.

## Configuration knobs

- `$enableAsyncCapability` controls the `ACALL` capability as described above.
- `setMaxProtocolVersion()` clamps to the supported range (v1–v2) and drives banner negotiation.

These behaviors are covered by the PHP integration tests under `php/tests/integration`, including payload negotiation, legacy clients, and ACALL capability coverage.
