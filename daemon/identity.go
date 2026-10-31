package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"

	libp2pcrypto "github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/peer"
	"golang.org/x/crypto/curve25519"
)

type Identity struct {
	mu        sync.RWMutex
	Nickname  string `json:"nickname"`
	Status    string `json:"status"`
	SeedHex   string `json:"seed_hex"`
	PubHex    string `json:"public_key"`
	XPubHex   string `json:"x25519_key"`

	seed      []byte
	edPriv    ed25519.PrivateKey
	edPub     ed25519.PublicKey
	xPriv     []byte
	xPub      []byte
	libp2pPriv libp2pcrypto.PrivKey
	peerID    peer.ID
}

func LoadOrCreateIdentity(dataDir, defaultNick string) (*Identity, error) {
	if err := os.MkdirAll(dataDir, 0700); err != nil {
		return nil, fmt.Errorf("mkdir data dir: %w", err)
	}

	jsonPath := filepath.Join(dataDir, "identity.json")
	binPath := filepath.Join(dataDir, "identity.key")

	var seed []byte
	nick := defaultNick
	status := "Online"

	// 1. Try reading existing JSON identity
	if data, err := os.ReadFile(jsonPath); err == nil {
		var stored struct {
			Nickname string `json:"nickname"`
			Status   string `json:"status"`
			SeedHex  string `json:"seed_hex"`
		}
		if err := json.Unmarshal(data, &stored); err == nil && len(stored.SeedHex) == 64 {
			if s, err := hex.DecodeString(stored.SeedHex); err == nil && len(s) == 32 {
				seed = s
				if stored.Nickname != "" {
					nick = stored.Nickname
				}
				if stored.Status != "" {
					status = stored.Status
				}
			}
		}
	}

	// 2. Fallback to existing C binary identity.key if present and no JSON
	if len(seed) == 0 {
		if raw, err := os.ReadFile(binPath); err == nil && len(raw) >= 40 {
			// In C disk struct: magic (4), version (2), reserved (2), seed (32)
			seed = make([]byte, 32)
			copy(seed, raw[8:40])
		}
	}

	// 3. Generate fresh seed if none found
	if len(seed) != 32 {
		seed = make([]byte, 32)
		if _, err := rand.Read(seed); err != nil {
			return nil, fmt.Errorf("crypto rand: %w", err)
		}
	}

	ident, err := NewIdentityFromSeed(seed, nick, status)
	if err != nil {
		return nil, err
	}

	_ = ident.Save(dataDir)
	return ident, nil
}

func NewIdentityFromSeed(seed []byte, nick, status string) (*Identity, error) {
	if len(seed) != 32 {
		return nil, fmt.Errorf("invalid seed length: %d (expected 32)", len(seed))
	}

	edPriv := ed25519.NewKeyFromSeed(seed)
	edPub := edPriv.Public().(ed25519.PublicKey)

	// Wrap in libp2p crypto
	lpPriv, err := libp2pcrypto.UnmarshalEd25519PrivateKey(edPriv)
	if err != nil {
		return nil, fmt.Errorf("unmarshal libp2p privkey: %w", err)
	}

	pid, err := peer.IDFromPrivateKey(lpPriv)
	if err != nil {
		return nil, fmt.Errorf("derive peer id: %w", err)
	}

	// Derive X25519 key for ChaCha20-Poly1305 E2EE
	xPriv := make([]byte, 32)
	copy(xPriv, seed)
	xPriv[0] &= 248
	xPriv[31] &= 127
	xPriv[31] |= 64
	xPub, err := curve25519.X25519(xPriv, curve25519.Basepoint)
	if err != nil {
		return nil, fmt.Errorf("derive x25519: %w", err)
	}

	if nick == "" {
		nick = "Sovereign"
	}
	if status == "" {
		status = "Online"
	}

	return &Identity{
		Nickname:   nick,
		Status:     status,
		SeedHex:    hex.EncodeToString(seed),
		PubHex:     hex.EncodeToString(edPub),
		XPubHex:    hex.EncodeToString(xPub),
		seed:       seed,
		edPriv:     edPriv,
		edPub:      edPub,
		xPriv:      xPriv,
		xPub:       xPub,
		libp2pPriv: lpPriv,
		peerID:     pid,
	}, nil
}

func (id *Identity) Save(dataDir string) error {
	id.mu.RLock()
	defer id.mu.RUnlock()

	data, err := json.MarshalIndent(id, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dataDir, "identity.json"), data, 0600)
}

func (id *Identity) Sign(msg []byte) []byte {
	id.mu.RLock()
	defer id.mu.RUnlock()
	return ed25519.Sign(id.edPriv, msg)
}

func (id *Identity) SetProfile(nick, status, dataDir string) {
	id.mu.Lock()
	if nick != "" {
		id.Nickname = nick
	}
	if status != "" {
		id.Status = status
	}
	id.mu.Unlock()
	_ = id.Save(dataDir)
}

func (id *Identity) GetProfile() (nick, status string) {
	id.mu.RLock()
	defer id.mu.RUnlock()
	return id.Nickname, id.Status
}

func VerifySignature(pubKeyHex, sigHex string, msg []byte) bool {
	pub, err := hex.DecodeString(pubKeyHex)
	if err != nil || len(pub) != 32 {
		return false
	}
	sig, err := hex.DecodeString(sigHex)
	if err != nil || len(sig) != 64 {
		return false
	}
	return ed25519.Verify(pub, msg, sig)
}
