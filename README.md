# KB Core

> High-performance, single-file embedded storage engine designed in C11 for hierarchical document trees, structured knowledge bases, and Personal Knowledge Management (PKM) systems.

[![Language](https://img.shields.io/badge/language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Standard](https://img.shields.io/badge/compiler-strict%20-Werror-success.svg)]()
[![Tests](https://img.shields.io/badge/tests-48%2C700%2B%20passing-brightgreen.svg)]()
[![Dependencies](https://img.shields.io/badge/dependencies-zero-orange.svg)]()

---

## Overview

**KB Core** is an embedded storage engine designed from first principles for hierarchical knowledge graphs and structured documents. Unlike generic key-value stores or SQL databases, KB Core is engineered around the structural properties of knowledge trees:

- **64-Byte Cache-Line Headers:** Core object headers are fixed at 64 bytes and never resize or move, allowing single-cache-line fetches.
- **Hierarchical Path Encoding:** Tree positions (`path_uid`) use prefix-length varint encoding where byte order identically preserves numeric hierarchical order (`/1/4/2`).
- **40-bit Unit References:** Addresses memory in 64-byte units using 5-byte references, supporting pools up to 64 TiB while saving 3 bytes per pointer over `uint64_t`.
- **Zero-mmap Explicit Buffer Pool:** User-space bounded buffer pool with LRU eviction and pin/lease semantics, eliminating OS page-fault stalls and signal overhead.
- **Slab Allocator with Deferred Reuse:** Power-of-2 size classes (64 B to 2048 B) for high-churn metadata blocks, with deferred reuse to completely prevent ABA pointer hazards.
- **WAL Durability & Group Commit:** Append-only Write-Ahead Log (WAL) ring buffer enforcing write-ahead durability, patch records, and crash recovery with CRC32C Castagnoli hardware acceleration.
- **Front-Coded B+ Tree Index:** Secondary index mapping tree paths to header addresses with prefix compression and balanced node splitting.
- **Full Invariant Verification:** Cross-subsystem integrity checker validating segment bitmaps, B+ tree balance, parent-child references, and payload extents.

---

## Architecture at a Glance

```
 ┌───────────────────────────────────────────────────────────────┐
 │               Public API Facade (kb_store.h)                 │
 └───────┬──────────────────────┬──────────────────────┬─────────┘
         │                      │                      │
 ┌───────▼────────┐     ┌───────▼────────┐     ┌───────▼─────────┐
 │ Hierarchy & ID │     │  Storage & IO  │     │ Index & Search  │
 │  - kb_path     │     │  - kb_pool     │     │  - kb_index     │
 │  - kb_child    │     │  - kb_buffer   │     │  - kb_block     │
 │  - kb_header   │     │  - kb_os_posix │     └─────────────────┘
 └───────┬────────┘     └───────┬────────┘             │
         │                      │                      │
 ┌───────▼────────┐     ┌───────▼────────┐             │
 │  Payload Data  │     │   Durability   │             │
 │  - kb_extent   │     │  - kb_wal      │◄────────────┘
 │  - kb_slab     │     │  - kb_crc32c   │
 └────────────────┘     └───────┬────────┘
                                │
                        ┌───────▼────────┐
                        │ Verify & Audit │
                        │  - kb_verify   │
                        └────────────────┘
```

For full specifications and technical decisions, see [plan-architecture-specification.md](plan-architecture-specification.md).

---

## Directory Structure

```
.
├── src/
│   └── core/                 # C11 Core Storage Engine
│       ├── kb_types.h        # Fundamental types, status codes, constants
│       ├── kb_store.h/.c     # Unified store facade API
│       ├── kb_header.h/.c    # 64-byte core headers
│       ├── kb_pool.h/.c      # File & segment allocation (pool.db)
│       ├── kb_slab.h/.c      # Power-of-2 slab allocator
│       ├── kb_buffer.h/.c    # User-space buffer pool & leases
│       ├── kb_index.h/.c     # Front-coded B+ tree index
│       ├── kb_wal.h/.c       # Write-ahead log & recovery
│       ├── kb_extent.h/.c    # Payload extents & fragment tags
│       ├── kb_child.h/.c     # Out-of-line child tables
│       ├── kb_path.h/.c      # Varint hierarchical path codec
│       ├── kb_crc32c.h/.c    # Castagnoli CRC32C checksums
│       ├── kb_verify.h/.c    # Invariant and integrity validator
│       └── kb_os_posix.c     # POSIX direct I/O, sync, fallocate
├── tests/                    # 13 Test Suites (>48,700 assertions)
│   ├── test_main.h           # Zero-dependency test harness
│   ├── test_path.c           # Path encoding & order tests
│   ├── test_crc32c.c         # CRC32C hardware/software tests
│   ├── test_superblock.c     # Superblock dual-slot atomic tests
│   ├── test_pool.c           # Segment & chunk allocator tests
│   ├── test_slab.c           # Slab allocator & deferred reuse
│   ├── test_header.c         # Header lifecycle & swap tests
│   ├── test_child.c          # Out-of-line child table tests
│   ├── test_extent.c         # Payload extent chaining tests
│   ├── test_wal.c            # WAL ring, group commit & recovery
│   ├── test_index.c          # B+ tree split/merge/lookup tests
│   ├── test_buffer.c         # Buffer pool LRU & pinning tests
│   ├── test_store.c          # End-to-end unified store tests
│   └── test_verify.c         # Full database audit & verify tests
├── tools/
│   ├── kb_inspect.c          # Interactive CLI inspector & hex dumper
│   ├── kb_server.c           # Embedded HTTP server (JSON REST API)
│   ├── embed_html.sh         # Resource embedding script for web GUI
│   └── web/
│       └── index.html        # Interactive dark-mode developer GUI
├── Makefile                  # Build system (Strict C11 / -Werror)
└── CMakeLists.txt            # CMake configuration
```

---

## Getting Started

### Prerequisites

- A C11-compliant C compiler (`clang` or `gcc`)
- Make
- POSIX environment (macOS, Linux)

### Building

Build the static library (`libkb_core.a`) and all test executables:

```bash
make
```

### Running the Test Suite

Execute all 13 test suites (over 48,700 assertions):

```bash
make test
```

### Developer Tools

#### 1. Interactive CLI Inspector
Inspect pool superblocks, headers, B+ tree nodes, slab allocators, and raw hex bytes:

```bash
make tools
./build/kb_inspect pool.db
```

#### 2. Visual Web Dashboard
Run the embedded HTTP server to explore and audit databases visually at `http://localhost:9090`:

```bash
make gui
```

---

## Design Principles

1. **Deterministic Layout:** Immovable 64-byte headers eliminate pointer invalidation. Out-of-line blocks resize via copy-and-swap transactions.
2. **Crash Resilience:** WAL ring buffer with group commit and CRC32C checksums guarantees atomicity and crash recovery.
3. **No Garbage Holes:** Low-address-first priority allocation reclaims free space systematically; freed blocks are reusable only after transactions become durable.
4. **Zero External Dependencies:** Built entirely with standard C11 and POSIX system calls.

---

## License

Copyright (c) 2026 Harsh. All rights reserved.

Source-available for evaluation, reading, and educational purposes. Commercial use, reproduction, distribution, and modification are prohibited without express prior written permission. See [LICENSE](LICENSE) for full details.

