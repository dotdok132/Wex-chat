package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	p2pPort := flag.Int("port", 9001, "P2P network listen port (QUIC + TCP)")
	wsPort := flag.Int("ws", 9000, "Web UI & WebSocket server port")
	nick := flag.String("nick", "dotdok", "User nickname")
	dataDir := flag.String("data-dir", "./.wex_data", "Directory for cryptographic keys and storage")
	bootstrap := flag.String("bootstrap", "", "Initial bootstrap multiaddr or peer to connect to")
	isSeed := flag.Bool("seed", false, "Run in headless Circuit Relay v2 seed mode")
	webDir := flag.String("web", "web", "Directory containing web UI files (index.html, wex_app.js)")

	flag.Parse()

	fmt.Println("==================================================")
	fmt.Println("    Wex-Chat Sovereign Daemon (libp2p Engine)     ")
	fmt.Println("==================================================")

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 1. Load or create persistent identity
	ident, err := LoadOrCreateIdentity(*dataDir, *nick)
	if err != nil {
		log.Fatalf("[-] Failed to load identity: %v", err)
	}

	fmt.Printf("[+] Identity Nickname: %s\n", ident.Nickname)
	fmt.Printf("[+] Ed25519 Public Key: %s\n", ident.PubHex)
	fmt.Printf("[+] libp2p Peer ID:    %s\n", ident.peerID.String())

	// 2. Initialize message & blob storage
	storage, err := NewStorage(*dataDir)
	if err != nil {
		log.Fatalf("[-] Failed to init storage: %v", err)
	}
	fmt.Printf("[+] Message storage initialized: %d messages in journal\n", len(storage.GetHistory()))

	// 3. Prepare bootstrap peers
	bootstrapPeers := make([]string, 0)
	if *bootstrap != "" {
		bootstrapPeers = append(bootstrapPeers, *bootstrap)
	}

	// 4. Initialize P2P Swarm with libp2p (QUIC, TCP, UPnP, DCUtR Hole-Punching)
	p2pNode, err := NewP2PNode(ctx, ident, storage, *p2pPort, *isSeed, bootstrapPeers)
	if err != nil {
		log.Fatalf("[-] Failed to initialize libp2p node: %v", err)
	}
	defer p2pNode.Close()

	fmt.Printf("[+] P2P Engine listening on QUIC & TCP port %d\n", *p2pPort)
	for _, addr := range p2pNode.GetExternalMultiaddrs() {
		fmt.Printf("    -> Multiaddr: %s\n", addr)
	}

	// 5. Start Web UI & WebSocket server
	server := NewServer(*wsPort, *webDir, *dataDir, ident, storage, p2pNode)

	// Graceful shutdown on SIGINT/SIGTERM
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigCh
		fmt.Println("\n[*] Shutting down Wex-Chat daemon gracefully...")
		cancel()
		p2pNode.Close()
		os.Exit(0)
	}()

	if err := server.Start(); err != nil {
		log.Fatalf("[-] Server fatal error: %v", err)
	}
}
