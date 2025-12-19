# ProfiWeb CloudRMI PHP bindings (v0)

This directory contains the thin PHP extension that exposes the libcloudrmi protocol engine and the accompanying Composer package (`php/cloudrmi`, namespace `ProfiWeb\CloudRMI`) with the PHP client/server harness used for testing the CALL + PAYLOAD event model.

## Building
1. Build libcloudrmi (static library):
   ```bash
   make -C ../../libcloudrmi
   ```
2. Build the PHP extension (from this folder):
   ```bash
   cd ext/profiweb_cloudrmi
   phpize
   ./configure --with-profiweb_cloudrmi=$(pwd)/../../../libcloudrmi
   make
   ```
   The module is built at `ext/profiweb_cloudrmi/modules/profiweb_cloudrmi.so`.

## Running tests
All commands should be executed from the repository root with the extension built as above.

### Unit tests (protocol engine)
```bash
make -C php unit
```

### Integration tests (PHP client/server over sockets)
```bash
make -C php integration
```

## Notes
- The PHP API mirrors libcloudrmi and exposes raw events; no high-level wrappers are added.
- Payload handling is binary safe (PHP strings preserve NUL bytes).
- Protocol limits remain unchanged; parser behavior is defined by libcloudrmi.
