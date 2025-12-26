# Cloud RMI

Cloud-RMI is an implementation of the ProfiWeb Cloud RMI protocol. This repository contains all 4 interconnected units in a monorepo:

1. **C Library** (`libcloudrmi/`) - Core protocol engine library
2. **C Implementation** (`src/`) - C binaries: `rmi-server`, `rmi-client`, `rmi-proxy`
3. **PHP Extension** (`php/ext/profiweb_cloudrmi/`) - PHP extension exposing libcloudrmi APIs
4. **PHP Composer Package** (`php/cloudrmi/`) - PHP userland library (`profiweb/cloud-rmi-php`)

See `docs/STRUCTURE.md` for details on the monorepo organization.

## Components Overview

### C Library (`libcloudrmi/`)

The core protocol engine providing byte-level protocol parsing, framing, state machine enforcement, and capability handling. This is the foundation used by both C binaries and the PHP extension.

### C Implementation (`src/`)

Command-line binaries:
- `rmi-server` - RMI protocol server
- `rmi-client` - RMI protocol client
- `rmi-proxy` - RMI protocol proxy

### PHP Extension (`php/ext/profiweb_cloudrmi/`)

PHP extension that exposes `libcloudrmi` APIs directly, enabling zero-copy payload paths and fast IPC (UNIX sockets/pipes). Build with `make php` or `make -C php build-ext`.

### PHP Composer Package (`php/cloudrmi/`)

PHP userland library (`ProfiWeb\CloudRMI` namespace) providing Cloud-RMI client/server functionality. Can work with or without the PHP extension. See `php/README.md` for PHP-specific documentation.

## Configuration
Example configuration files are located in `config` directory

## Building

Build process is automated using Makefiles. From the repository root:

- `make` - Build C library and C binaries (`rmi-server`, `rmi-client`, `rmi-proxy`)
- `make php` - Build PHP extension (requires C library to be built first)
- `make install` - Install C binaries to system
- `make php_install` - Build and install PHP extension

## Testing

Run tests from the repository root:

- `make test` - Run C library unit tests and C integration tests
- `make test-all` - Run all test suites (C library unit tests, C integration tests, PHP unit tests, PHP integration tests)
- `make -C libcloudrmi/tests/unit` - Run only C library unit tests
- `make -C tests/integration test` - Run only C integration tests
- `make -C php test` - Run only PHP tests (requires PHP extension to be built)

#### Makefile Changelog
- Added new gcc parameter "-lm" to link math library, in order for the code to work on modern systems

#### Compile Options 
Add after `gcc` command using `-D` argument (e.g. `gcc ... -D PROXY_SUPPORT`)
 
- PROXY_SUPPORT			- enable proxy support for server
- ENABLE_SYSLOG_TRACE   - enable syslog and `/tmp/rmi-server.log` writes (defaults to `1`)
- ENABLE_DEBUG_LOGGING  - enable debug logging support

## Logging and debugging controls

Runtime verbosity can be toggled without recompiling:

- `debug`/`-v` enable the existing verbose mode.
- `debug_traffic`/`-T` enables lightweight traffic summaries (request/response framing only) routed through `log_event`.
- `SERVER DEBUG on|off` enable verbose mode lat runtime.
- `SERVER DEBUG TRAFFIC on|off` can be issued on an authenticated session to flip traffic logging at runtime.

Traffic summaries elide payload bytes and are bounded to protect protocol framing; they are useful when chasing crashes and can be disabled for steady-state performance.

## Development
Works with standard gcc and make. Make sure to read Notes section before commiting any changes.

## Notes
- The server uses a fork-per-connection worker model

## ProfiWeb VPN
For development and testing there are these RMI ports currently available on ProfiWeb VPN:
10.100.0.1	9501	services.email.dkim.rmi (courier)
10.100.0.1	9503	services.crypto.rmi (osiris)

Example proxy configuration:
```
proxy		= 10.100.0.1:9501, 10.100.0.1:9503
```


## TODO
