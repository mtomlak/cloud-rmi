# Cloud-RMI Project Structure

This document explains the monorepo structure for all 4 interconnected units of the Cloud-RMI project.

## Repository Overview

The repository contains all 4 units in a single monorepo to facilitate development, testing, and agent automation:

```
cloud-rmi/
├── libcloudrmi/          # 1. C library (core protocol engine)
├── src/                   # 2. C implementation (client, server, proxy)
├── php/
│   ├── ext/              # 3. PHP extension
│   └── cloudrmi/         # 4. PHP composer package
├── tests/                 # Integration tests (Python harness)
├── php/tests/            # PHP-specific tests
└── docs/                 # Documentation
```

All 4 units are kept together because they are interconnected parts needed during testing and development workflows.

---

## Component Details

### 1. C Library (`libcloudrmi/`)

The core protocol engine library.

- **Location:** `libcloudrmi/`
- **Build artifact:** `libcloudrmi/libcloudrmi.a` (static library)
- **Headers:** `libcloudrmi/include/cloudrmi/`
- **Source:** `libcloudrmi/src/`
- **Tests:** `libcloudrmi/tests/unit/`

**Dependencies:**
- None (core protocol engine)

**Used by:**
- C client/server/proxy (`src/`)
- PHP extension (`php/ext/profiweb_cloudrmi/`)

**Build integration:**
- Built via `libcloudrmi/Makefile`
- Root `Makefile` builds it as dependency
- PHP extension build system references it

---

### 2. C Implementation (`src/`)

C binaries for client, server, and proxy.

- **Location:** `src/`
- **Binaries:** `rmi-server`, `rmi-client`, `rmi-proxy` (root level)
- **Components:**
  - `server_main.c`, `server.c` → `rmi-server`
  - `client_main.c`, `client.c` → `rmi-client`
  - `proxy.c` + server/client code → `rmi-proxy`

**Dependencies:**
- `libcloudrmi` (via `-Llibcloudrmi -lcloudrmi`)

**Build integration:**
- Root `Makefile` orchestrates build
- All binaries depend on `libcloudrmi.a`
- Version header generated from git tags

---

### 3. PHP Extension (`php/ext/profiweb_cloudrmi/`)

PHP extension that exposes `libcloudrmi` APIs.

- **Location:** `php/ext/profiweb_cloudrmi/`
- **Build artifact:** `php/ext/profiweb_cloudrmi/modules/profiweb_cloudrmi.so`
- **Source:** `php/ext/profiweb_cloudrmi/cloudrmi.c`
- **Config:** `php/ext/profiweb_cloudrmi/config.m4`

**Dependencies:**
- `libcloudrmi` (configured via `--with-profiweb_cloudrmi=../../libcloudrmi`)

**Build integration:**
- `php/Makefile` orchestrates extension build
- Requires `libcloudrmi` to be built first
- Uses standard PHP extension build system

---

### 4. PHP Composer Package (`php/cloudrmi/`)

PHP userland library providing Cloud-RMI client/server functionality.

- **Location:** `php/cloudrmi/`
- **Package name:** `profiweb/cloud-rmi-php`
- **Namespace:** `ProfiWeb\CloudRMI`
- **Source:** `php/cloudrmi/src/`
- **Tests:** `php/tests/` (unit + integration)

**Dependencies:**
- PHP 8.1+
- `ext-pcntl`, `ext-posix`
- Can optionally use PHP extension (but works without it)

**Build integration:**
- `composer.json` defines package
- `php/Makefile` handles `composer install`
- Tests can run with or without extension

---

## Build Dependencies & Order

### Build Order

1. `libcloudrmi` → `libcloudrmi.a`
2. C binaries (`rmi-server`, `rmi-client`, `rmi-proxy`)
3. PHP extension (depends on `libcloudrmi`)
4. PHP composer package (no build step, just `composer install`)

### Dependency Graph

```
libcloudrmi (C library)
    ├──→ src/ (C binaries)
    └──→ php/ext/ (PHP extension)
            └──→ php/cloudrmi/ (PHP package can use extension)
```

---

## Test Organization

The repository contains multiple test suites organized by component:

### 1. C Library Unit Tests
- **Location:** `libcloudrmi/tests/unit/`
- **Purpose:** Protocol engine tests
- **Run:** `make -C libcloudrmi/tests/unit`

### 2. C Integration Tests
- **Location:** `tests/integration/`
- **Purpose:** Tests C server/client/proxy using Python harness
- **Run:** `make -C tests/integration test`
- **Fixtures:** `tests/integration/fixtures/`

### 3. PHP Tests
- **Location:** `php/tests/`
- **Unit tests:** `php/tests/unit/`
- **Integration tests:** `php/tests/integration/`
- **Run:** `make -C php test` (requires extension build)

### Running All Tests

From the root directory:
- `make test` - Runs C library unit tests and C integration tests
- `make test-all` - Runs all test suites (C and PHP)

See the [Testing](#testing) section below for details.

---

## Build Artifacts

Build artifacts are excluded from version control via `.gitignore`:

- C binaries: `rmi-server`, `rmi-client`, `rmi-proxy`
- Object files: `*.o`
- Static libraries: `*.a`
- Shared libraries: `*.so`, `*.so.*`, `*.dylib`
- Test executables
- Python virtual environments
- PHP vendor directory
- Generated version header (`src/app_version.h`)
- PHP extension build artifacts (handled by `php/ext/profiweb_cloudrmi/.gitignore`)

---

## Documentation

- `docs/GUIDELINES.md` - Development guidelines (applies to all components)
- `docs/PROTOCOL.md` - Protocol specification (applies to all components)
- `docs/IMPLEMENTATION.md` - Implementation details
- `docs/STRUCTURE.md` - This document (monorepo structure)
- `README.md` - Root README with overview of all components
- `php/README.md` - PHP-specific documentation
- `tests/integration/README.md` - Integration test documentation

---

## Why Monorepo?

All 4 units are kept in one repository because:

1. **Interconnected parts** - They depend on each other (C library → C binaries, PHP extension)
2. **Testing requirements** - Integration tests need all components
3. **Agent workflows** - Easier to work with when all code is visible
4. **Unified development** - Protocol changes affect multiple components
5. **Version synchronization** - Related components evolve together

The monorepo structure supports:
- Clear component boundaries
- Proper dependency management
- Unified testing infrastructure
- Agent-friendly workflows
- Comprehensive documentation

