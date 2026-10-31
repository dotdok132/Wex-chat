package main

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"
)

func TestP2PEndToEnd(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	dirA := "/tmp/wex_test_node_a"
	dirB := "/tmp/wex_test_node_b"
	_ = os.RemoveAll(dirA)
	_ = os.RemoveAll(dirB)
	defer os.RemoveAll(dirA)
	defer os.RemoveAll(dirB)

	// Node A
	identA, err := LoadOrCreateIdentity(dirA, "Alice")
	if err != nil {
		t.Fatalf("identity A: %v", err)
	}
	storeA, err := NewStorage(dirA)
	if err != nil {
		t.Fatalf("storage A: %v", err)
	}
	nodeA, err := NewP2PNode(ctx, identA, storeA, 0, false, nil)
	if err != nil {
		t.Fatalf("node A: %v", err)
	}
	defer nodeA.Close()

	// Node B
	identB, err := LoadOrCreateIdentity(dirB, "Bob")
	if err != nil {
		t.Fatalf("identity B: %v", err)
	}
	storeB, err := NewStorage(dirB)
	if err != nil {
		t.Fatalf("storage B: %v", err)
	}
	nodeB, err := NewP2PNode(ctx, identB, storeB, 0, false, nil)
	if err != nil {
		t.Fatalf("node B: %v", err)
	}
	defer nodeB.Close()

	// Connect B to A directly via multiaddr
	addrsA := nodeA.GetExternalMultiaddrs()
	if len(addrsA) == 0 {
		t.Fatalf("no multiaddrs for A")
	}

	if err := nodeB.ConnectPeer(addrsA[0]); err != nil {
		t.Fatalf("failed to connect B to A: %v", err)
	}

	// Wait briefly for GossipSub mesh to link
	time.Sleep(1 * time.Second)

	msgReceived := make(chan Message, 1)
	nodeB.SetCallbacks(func(m Message) {
		msgReceived <- m
	}, nil)

	testMsg := Message{
		Type:        "message",
		Author:      "Alice",
		AuthorPK:    identA.PubHex,
		Channel:     "general",
		Content:     "Hello from libp2p test!",
		Verified:    true,
		TimestampMS: time.Now().UnixMilli(),
		Signature:   "fakesig123",
		SpaceID:     DefaultSpaceHex,
	}

	if err := nodeA.PublishMessage(DefaultSpaceHex, "general", testMsg); err != nil {
		t.Fatalf("failed to publish: %v", err)
	}

	select {
	case received := <-msgReceived:
		if received.Content != testMsg.Content {
			t.Fatalf("content mismatch: got %s, want %s", received.Content, testMsg.Content)
		}
		if received.Author != "Alice" {
			t.Fatalf("author mismatch: got %s, want Alice", received.Author)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("timed out waiting for GossipSub message")
	}
}

func TestDirectMessageEndToEnd(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	dirA := "/tmp/wex_test_dm_a"
	dirB := "/tmp/wex_test_dm_b"
	_ = os.RemoveAll(dirA)
	_ = os.RemoveAll(dirB)
	defer os.RemoveAll(dirA)
	defer os.RemoveAll(dirB)

	identA, _ := LoadOrCreateIdentity(dirA, "Alice")
	storeA, _ := NewStorage(dirA)
	nodeA, _ := NewP2PNode(ctx, identA, storeA, 0, false, nil)
	defer nodeA.Close()

	identB, _ := LoadOrCreateIdentity(dirB, "Bob")
	storeB, _ := NewStorage(dirB)
	nodeB, _ := NewP2PNode(ctx, identB, storeB, 0, false, nil)
	defer nodeB.Close()

	// Connect B to A
	addrsA := nodeA.GetExternalMultiaddrs()
	if err := nodeB.ConnectPeer(addrsA[0]); err != nil {
		t.Fatalf("connect failed: %v", err)
	}

	dmReceived := make(chan Message, 1)
	nodeB.SetCallbacks(func(m Message) {
		dmReceived <- m
	}, nil)

	time.Sleep(1 * time.Second)

	dmMsg := Message{
		Type:        "message",
		Author:      "Alice",
		AuthorPK:    identA.PubHex,
		PeerPK:      identB.PubHex,
		Channel:     "dm:alice",
		Content:     "Secret direct ping via libp2p stream!",
		Verified:    true,
		Encrypted:   true,
		Signature:   "fakedmsig",
		TimestampMS: time.Now().UnixMilli(),
	}

	// Send direct message using Bob's PeerID
	if err := nodeA.SendDirectMessage(identB.peerID.String(), dmMsg); err != nil {
		t.Fatalf("failed to send DM: %v", err)
	}

	select {
	case received := <-dmReceived:
		if received.Content != dmMsg.Content {
			t.Fatalf("dm content mismatch: got %s, want %s", received.Content, dmMsg.Content)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("timed out waiting for DM stream message")
	}
}

func TestMultiEndpointSTUNAndInvite(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	dir := "/tmp/wex_test_multi_ep"
	_ = os.RemoveAll(dir)
	defer os.RemoveAll(dir)

	ident, err := LoadOrCreateIdentity(dir, "MultiEndpointUser")
	if err != nil {
		t.Fatalf("identity: %v", err)
	}
	store, err := NewStorage(dir)
	if err != nil {
		t.Fatalf("storage: %v", err)
	}
	node, err := NewP2PNode(ctx, ident, store, 0, false, nil)
	if err != nil {
		t.Fatalf("node: %v", err)
	}
	defer node.Close()

	srv := NewServer(0, "web", dir, ident, store, node)
	status := srv.buildStatus()

	if status.Identity.LANEndpoint == "" {
		t.Fatalf("LANEndpoint should not be empty")
	}
	if !strings.Contains(status.Identity.InviteURI, "wan=") {
		t.Fatalf("InviteURI missing wan=: %s", status.Identity.InviteURI)
	}
	if !strings.Contains(status.Identity.InviteURI, "lan=") {
		t.Fatalf("InviteURI missing lan=: %s", status.Identity.InviteURI)
	}
	if !strings.Contains(status.Identity.InviteURI, "peer=") {
		t.Fatalf("InviteURI missing peer=: %s", status.Identity.InviteURI)
	}
	if !strings.Contains(status.Identity.InviteURI, "pk=") {
		t.Fatalf("InviteURI missing pk=: %s", status.Identity.InviteURI)
	}

	lan := DetectLocalLANIP(9005)
	if !strings.HasSuffix(lan, ":9005") {
		t.Fatalf("DetectLocalLANIP unexpected result: %s", lan)
	}
}
