# ProfiWeb CloudRMI PHP bindings (v0)

This directory contains the thin PHP extension that exposes the libcloudrmi protocol engine and the accompanying PHP client/server harness used for testing the CALL + PAYLOAD event model.

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
php -d extension=php/ext/profiweb_cloudrmi/modules/profiweb_cloudrmi.so php/tests/unit/protocol.php
```

### Integration tests (PHP client/server over sockets)
```bash
php -d extension=php/ext/profiweb_cloudrmi/modules/profiweb_cloudrmi.so php/tests/integration/cloudrmi_integration.php
```

## Notes
- The PHP API mirrors libcloudrmi and exposes raw events; no high-level wrappers are added.
- Payload handling is binary safe (PHP strings preserve NUL bytes).
- Protocol limits remain unchanged; parser behavior is defined by libcloudrmi.
