# Wex-Chat

> Sovereign, anti-surveillance, peer-to-peer messaging daemon.

Wex-Chat is an uncompromising privacy-first communication protocol designed to eliminate reliance on centralized platforms (such as Discord). It mandates zero identity verification, zero phone numbers, and operates entirely over an ephemeral, decentralized mesh.

---

## Key Security Features

- **No Central Servers / Zero KYC**: No accounts, phone numbers, email addresses, or central coordination servers. Identities are Ed25519 cryptographic keypairs.
- **Multi-Path Shamir's Secret Sharing (SSS)**:
  - Each message key is partitioned into $(k, n)$ secret shares using polynomial secret sharing over $GF(2^8)$.
  - Shares are distributed across disjoint multi-route DHT paths. Intercepting fewer than $k$ shares reveals mathematically zero information about the message.
- **Traffic Analysis Defense (Constant-Size Framing)**:
  - All transport packets are padded to a strictly constant size (1024 bytes) using cryptographically secure random padding.
  - Defeats length-based fingerprinting and metadata surveillance.
- **Store-and-Forward**:
  - Ephemeral distributed relay nodes temporarily buffer encrypted shards for offline peers.
  - Proof-of-Work (PoW) and strict TTL prevent denial-of-service and buffer saturation.
- **Ultra-Lean C99 Engine**:
  - Minimal resource footprint, low latency, and zero bulky runtime dependencies (no Node.js/Python).

---

## Architecture Overview

```
                      +-------------------+
                      | Outgoing Message  |
                      +---------+---------+
                                |
               +----------------v----------------+
               | ChaCha20-Poly1305 Encrypted Msg |
               +----------------+----------------+
                                |
             +------------------v------------------+
             | Shamir Secret Sharing (k-of-n) Split|
             +------------------+------------------+
                                |
            +-------------------+-------------------+
            |                   |                   |
     [ Share 1 ]         [ Share 2 ]         [ Share N ]
            |                   |                   |
    [ Pad to 1024B ]    [ Pad to 1024B ]    [ Pad to 1024B ]
            |                   |                   |
            v                   v                   v
      ( Route A )         ( Route B )         ( Route N )
            \                   |                   /
             +------------------+------------------+
                                |
                   +------------v------------+
                   |  Recipient Node Reasms  |
                   |  (k shares -> Plaintext)|
                   +-------------------------+
```

---

## Directory Structure

```text
.
├── CMakeLists.txt          # Root build configuration
├── GEMINI.md               # Agent & developer guidelines
├── include/                # Public C header interfaces
│   ├── wex_crypto.h        # Key exchange & Shamir secret sharing
│   ├── wex_packet.h        # Fixed-size packet framing & padding
│   ├── wex_dht.h           # Distributed hash table & routing
│   └── wex_net.h           # Non-blocking network I/O
├── src/                    # Implementation sources
│   ├── crypto/             # Crypto engine & SSS
│   ├── packet/             # Serialization & padding
│   ├── dht/                # Kademlia-inspired routing table
│   ├── storage/            # Store-and-forward buffer
│   ├── net/                # Event loop & UDP transport
│   └── main.c              # Daemon entry point
└── tests/                  # Unit and integration test suites
```

---

## Building from Source

### Prerequisites
- C99 compliant compiler (`gcc` or `clang`)
- `cmake` (version 3.16+)
- `make` or `ninja`

### Compilation
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

### Running Tests
```bash
cd build
ctest --output-on-failure
```

---

## License
MIT
