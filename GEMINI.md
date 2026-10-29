# Wex-Chat: Developer & Agent Guidelines

## 1. Project Overview
**Wex-Chat** is an anti-surveillance, privacy-first, decentralized peer-to-peer (P2P) messaging platform.
It is built as an uncompromising, sovereign alternative to centralized walled gardens (like Discord), requiring **zero KYC, no phone numbers, and no central servers**.

### Key Architectural Pillars:
- **P2P Topology**: DHT (Kademlia-inspired) for node discovery and distributed routing.
- **Store-and-Forward**: Intermediate nodes hold encrypted packet shards for offline recipients with TTL and PoW (Proof-of-Work) to mitigate spam.
- **Multi-Path Shamir's Secret Sharing (SSS)**: Every message encryption key is split into $(k, n)$ threshold shares. Each share is routed across distinct peer paths, neutralizing single-node interception and traffic correlation.
- **Traffic Analysis Resistance**: Fixed-size packets (uniform padding, e.g., 1024 bytes) to prevent packet-size fingerprinting.
- **High Performance & Lean Runtime**: Pure C99 core daemon, non-blocking asynchronous event loop, zero runtime overhead.

---

## 2. Code & Safety Standards (C99)
- **Standard**: Strict `C99` (`-std=c99 -Wall -Wextra -Wpedantic -Werror`).
- **Memory Safety**:
  - Always check pointers and return values for allocations (`malloc`/`calloc`).
  - Zero sensitive memory buffers (`sodium_memzero` or `explicit_bzero`) before deallocating.
  - Compile and test with AddressSanitizer (`-fsanitize=address,undefined`) by default in debug builds.
  - No undefined behavior, buffer overflows, or unchecked array bounds.
- **Project Structure**:
  - `include/`: Public API headers.
  - `src/crypto/`: Key exchange, ChaCha20-Poly1305, Shamir Secret Sharing.
  - `src/packet/`: Fixed-size framing, padding, serialization.
  - `src/dht/`: Routing table, node identity, metric distance.
  - `src/storage/`: Store-and-forward transient cache.
  - `src/net/`: Socket I/O and event loop.
  - `tests/`: Automated unit tests for all modules.

---

## 3. Communication & Conventions
- All internal code comments and docstrings should be in clear English.
- Chat with the developer in Russian.
- Commit messages follow Conventional Commits (`feat:`, `fix:`, `refactor:`, `test:`, `docs:`).
