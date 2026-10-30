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

```text
       [ Web / Desktop UI ]  <--- WebSocket JSON-RPC / IPC ---> [ Wex-Chat Daemon ]
                                                                       |
       +---------------------------------------------------------------+----------------------------------------+
       |                                                               |                                        |
 [ Exort (Crypto) ]                                             [ Wex (P2P Network) ]                   [ Quas (Storage) ]
 - X25519 ECDH Key Exchange                                     - Kademlia DHT (alpha=3)                - Atomic Identity Key
 - ChaCha20-Poly1305 E2EE                                       - STUN NAT Traversal (RFC 5389)         - Append-Only Journal
 - Ed25519 Signatures                                           - Coordinated Hole Punching             - Chunked P2P Blobs
 - Shamir Secret Sharing (SSS)                                  - Fixed MTU & Voice Framing             - Store-and-Forward
```

---

## Directory Structure

```text
.
├── CMakeLists.txt          # Root build configuration
├── GEMINI.md               # Developer & agent guidelines
├── wex.conf.example        # Reference configuration file
├── include/                # Public C header interfaces
│   ├── wex_crypto.h        # Key exchange, ChaCha20-Poly1305, Shamir SSS
│   ├── wex_models.h        # Identities, Spaces, Channels, Chat messages
│   ├── wex_p2p.h           # P2P engine, mesh packet wire protocol, DMs
│   ├── wex_dht.h           # Kademlia routing table, lookup, topic providers
│   ├── wex_nat.h           # STUN RFC 5389 resolver & NAT hole punching
│   ├── wex_persist.h       # Atomic identity & binary journal persistence
│   ├── wex_blob.h          # Chunked P2P file/avatar engine & BLAKE2b
│   ├── wex_config.h        # Configuration file parser (wex.conf)
│   ├── wex_voice.h         # Voice framing, 48kHz RTP clock, jitter buffer
│   └── wex_ws_server.h     # High-speed non-blocking WebSocket & HTTP server
├── src/                    # Implementation sources
│   ├── crypto/             # Crypto engine & key derivation
│   ├── models/             # Space/channel models & config parser
│   ├── dht/                # Kademlia DHT routing table & iterative lookup
│   ├── storage/            # Disk persistence, message journal, blob storage
│   ├── net/                # STUN traversal, P2P mesh, voice, WebSocket server
│   └── main.c              # Unified daemon entry point (seed or client)
├── web/                    # Discord-like responsive Web UI
│   └── index.html          # Spaces, channels, DMs, file attachments, previews
└── tests/                  # Automated unit and integration test suites
```

---

## Building from Source

### Prerequisites
- C99 compliant compiler (`gcc` or `clang`)
- `cmake` (version 3.16+)
- `make` or `ninja`

### Compilation
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Running All 12 Automated Test Suites
```bash
ctest --test-dir build --output-on-failure
```

### Running the Daemon
```bash
# Run with Web UI on http://127.0.0.1:9000
./build/wex_daemon --ws 9000 --port 9001 --nick Alice

# Or run with a custom configuration file:
./build/wex_daemon --config wex.conf

# Or run as a headless Internet Seed & Relay router:
./build/wex_daemon --seed --port 9001
```
```

---

## License
MIT
