# Wex-Chat: Developer & Agent Guidelines

## 1. Project Overview
**Wex-Chat** is an everyday, privacy-first, sovereign alternative to Discord.
It solves real developer and user frustration: **zero KYC, no phone numbers, no age-verification passport scans, no corporate account bans, and no central servers**.

### Product Philosophy (Discord-like, NOT Darknet Paranoia):
- **Everyday Usability First**: Fast, lightweight, responsive, and intuitive (like Discord).
- **Not for evading state intelligence services**: Avoid cumbersome over-engineering (no CPU-burning Proof-of-Work on normal messages, no artificial packet jitter, no crippling 7KB limits).
- **Core Entities**:
  - Identity: Cryptographic keypair + human nickname and avatar (no phone/email).
  - Spaces/Guilds: Decentralized communities with channels (text, general, dev, etc.).
  - Real-time messaging: Instant delivery via P2P/direct routes, encrypted via ChaCha20-Poly1305.
  - Client API: WebSockets / IPC JSON-RPC for a beautiful modern web or desktop UI.

### Triad Taxonomy (Quas / Wex / Exort):
- **Exort (Power & Crypto)**: Cryptographic engine, keys (X25519/Ed25519), ChaCha20-Poly1305 encryption, and Shamir's Secret Sharing (SSS).
- **Wex (Speed & Flow)**: Network layer, P2P DHT routing, multi-path dispatching, uniform fixed-size frame transport.
- **Quas (Stasis & Retention)**: Store-and-Forward engine, offline message caching, TTL management, PoW spam defense.

---

## 2. Interaction Protocol with Developer
- **Rule of Consultation**: BEFORE implementing any major module or making architectural decisions, ALWAYS ask the developer how they want it to be done.
- Provide simple, high-level choices with trade-offs so the developer doesn't need to know low-level details to make informed product decisions.
- Maintain a proactive, pair-programming mindset: propose solutions, explain pros/cons, and execute only upon developer confirmation.

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
