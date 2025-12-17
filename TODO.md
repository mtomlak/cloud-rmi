# TODO

- Harden authentication pathways: the development backdoor (`AUTH dbg`) should be
  gated or removed, and token comparison should make failure reasons auditable
  without leaking secrets.
- Normalize capability parsing in one shared helper (library or common module)
  so the server, client, and proxy cannot drift on token handling or banner
  formats.
- Preserve encoding metadata when the proxy downshifts a payload response for
  v1 clients; today the base64 fallback omits the encoding token.
- Expand automated tests around parser overflow handling and zero-length
  payloads to lock in the documented limits (line ceiling, total buffer, and
  PAYLOAD sequencing).
- Move duplicated response framing logic (payload vs inline) from the server and
  proxy into a shared library utility to avoid inconsistencies in future
  changes.
