package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// Helper to set up an isolated Server instance for testing
func setupTestServer(t *testing.T) (*Server, string, string, string, func()) {
	t.Helper()

	dataDir, err := os.MkdirTemp("", "wex_server_test_*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}

	ident, err := LoadOrCreateIdentity(dataDir, "TestUser")
	if err != nil {
		_ = os.RemoveAll(dataDir)
		t.Fatalf("failed to create identity: %v", err)
	}

	storage, err := NewStorage(dataDir)
	if err != nil {
		_ = os.RemoveAll(dataDir)
		t.Fatalf("failed to create storage: %v", err)
	}

	webDir := filepath.Join(dataDir, "web")
	if err := os.MkdirAll(webDir, 0755); err != nil {
		_ = os.RemoveAll(dataDir)
		t.Fatalf("failed to create web dir: %v", err)
	}
	indexContent := "<html><body>Wex-Chat Web UI QA Test</body></html>"
	if err := os.WriteFile(filepath.Join(webDir, "index.html"), []byte(indexContent), 0644); err != nil {
		_ = os.RemoveAll(dataDir)
		t.Fatalf("failed to write test index.html: %v", err)
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		_ = os.RemoveAll(dataDir)
		t.Fatalf("failed to bind port 0: %v", err)
	}

	server := NewServer(0, webDir, dataDir, ident, storage, nil)
	go func() {
		_ = server.Serve(ln)
	}()

	addr := ln.Addr().String()
	wsURL := fmt.Sprintf("ws://%s/ws", addr)

	cleanup := func() {
		_ = server.Close()
		_ = os.RemoveAll(dataDir)
	}

	return server, addr, wsURL, dataDir, cleanup
}

// Helper to read the next JSON message with timeout
func readWSJSON(t *testing.T, conn *websocket.Conn, target interface{}, timeout time.Duration) {
	t.Helper()
	done := make(chan error, 1)
	go func() {
		_, p, err := conn.ReadMessage()
		if err != nil {
			done <- err
			return
		}
		done <- json.Unmarshal(p, target)
	}()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("failed to read/unmarshal WS JSON: %v", err)
		}
	case <-time.After(timeout):
		t.Fatalf("timed out waiting for WS message after %v", timeout)
	}
}

// Helper to read the raw message bytes with timeout
func readWSRaw(t *testing.T, conn *websocket.Conn, timeout time.Duration) []byte {
	t.Helper()
	res := make(chan []byte, 1)
	errCh := make(chan error, 1)
	go func() {
		_, p, err := conn.ReadMessage()
		if err != nil {
			errCh <- err
			return
		}
		res <- p
	}()

	select {
	case p := <-res:
		return p
	case err := <-errCh:
		t.Fatalf("failed to read raw WS message: %v", err)
		return nil
	case <-time.After(timeout):
		t.Fatalf("timed out waiting for raw WS message after %v", timeout)
		return nil
	}
}

// 1. Test that initial get_status returns an empty spaces array [] (no fake default servers)
// and that Voice status starts with connected: false, and HTTP static server works.
func TestInitialGetStatusAndVoiceStatus(t *testing.T) {
	_, addr, wsURL, _, cleanup := setupTestServer(t)
	defer cleanup()

	// 1. Verify HTTP static server serves index.html
	httpResp, err := http.Get(fmt.Sprintf("http://%s/", addr))
	if err != nil {
		t.Fatalf("HTTP GET root failed: %v", err)
	}
	defer httpResp.Body.Close()
	if httpResp.StatusCode != http.StatusOK {
		t.Fatalf("HTTP GET status code = %d, want 200", httpResp.StatusCode)
	}
	body, _ := io.ReadAll(httpResp.Body)
	if !strings.Contains(string(body), "Wex-Chat Web UI QA Test") {
		t.Fatalf("unexpected HTTP body: %s", string(body))
	}

	// 2. Connect with a real WebSocket client
	wsConn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("websocket dial failed: %v", err)
	}
	defer wsConn.Close()

	// Upon connecting, server automatically sends initial status
	rawInitial := readWSRaw(t, wsConn, 2*time.Second)

	// Ensure raw JSON contains "spaces":[] rather than null or pre-populated servers
	if !strings.Contains(string(rawInitial), `"spaces":[]`) {
		t.Fatalf("expected raw JSON to contain '\"spaces\":[]', got: %s", string(rawInitial))
	}

	var status StatusResponse
	if err := json.Unmarshal(rawInitial, &status); err != nil {
		t.Fatalf("failed to parse initial status: %v", err)
	}

	if status.Type != "status" {
		t.Fatalf("status.Type = %s, want 'status'", status.Type)
	}
	if len(status.Spaces) != 0 {
		t.Fatalf("initial spaces count = %d, expected 0 (no fake default servers)", len(status.Spaces))
	}
	if status.Voice.Connected {
		t.Fatalf("initial voice.connected = true, expected false")
	}
	if status.Voice.Channel != "" {
		t.Fatalf("initial voice.channel = %q, expected empty string", status.Voice.Channel)
	}

	// 3. Send explicit get_status action
	if err := wsConn.WriteJSON(IncomingAction{Action: "get_status"}); err != nil {
		t.Fatalf("failed to send get_status: %v", err)
	}

	var explicitStatus StatusResponse
	readWSJSON(t, wsConn, &explicitStatus, 2*time.Second)

	if explicitStatus.Type != "status" {
		t.Fatalf("explicitStatus.Type = %s, want 'status'", explicitStatus.Type)
	}
	if len(explicitStatus.Spaces) != 0 {
		t.Fatalf("explicit get_status spaces count = %d, expected 0", len(explicitStatus.Spaces))
	}
	if explicitStatus.Voice.Connected {
		t.Fatalf("explicit get_status voice.connected = true, expected false")
	}
}

// 2. Test that creating spaces and channels works and updates the status dynamically
func TestCreateSpaceAndChannel(t *testing.T) {
	_, _, wsURL, _, cleanup := setupTestServer(t)
	defer cleanup()

	wsConn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial failed: %v", err)
	}
	defer wsConn.Close()

	// Drain initial status
	var initStatus StatusResponse
	readWSJSON(t, wsConn, &initStatus, 2*time.Second)
	if len(initStatus.Spaces) != 0 {
		t.Fatalf("expected 0 initial spaces, got %d", len(initStatus.Spaces))
	}

	// Create Space 1: "Sovereign Devs"
	if err := wsConn.WriteJSON(IncomingAction{
		Action: "create_space",
		Name:   "Sovereign Devs",
	}); err != nil {
		t.Fatalf("failed to send create_space: %v", err)
	}

	var statusAfterCreate StatusResponse
	readWSJSON(t, wsConn, &statusAfterCreate, 2*time.Second)

	if len(statusAfterCreate.Spaces) != 1 {
		t.Fatalf("expected 1 space after create_space, got %d", len(statusAfterCreate.Spaces))
	}
	sp := statusAfterCreate.Spaces[0]
	if sp.Name != "Sovereign Devs" {
		t.Fatalf("space name = %s, want 'Sovereign Devs'", sp.Name)
	}
	if len(sp.ID) != 64 {
		t.Fatalf("space ID length = %d, want 64 (hex sha256)", len(sp.ID))
	}
	if len(sp.Channels) < 3 {
		t.Fatalf("expected default channels (general, dev, voice-lounge), got: %v", sp.Channels)
	}

	// Create Channel: "architecture"
	if err := wsConn.WriteJSON(IncomingAction{
		Action: "create_channel",
		Name:   "architecture",
	}); err != nil {
		t.Fatalf("failed to send create_channel: %v", err)
	}

	var statusAfterCh StatusResponse
	readWSJSON(t, wsConn, &statusAfterCh, 2*time.Second)

	if len(statusAfterCh.Spaces) != 1 {
		t.Fatalf("expected 1 space, got %d", len(statusAfterCh.Spaces))
	}
	foundCh := false
	for _, ch := range statusAfterCh.Spaces[0].Channels {
		if ch == "architecture" {
			foundCh = true
			break
		}
	}
	if !foundCh {
		t.Fatalf("channel 'architecture' not found in channels: %v", statusAfterCh.Spaces[0].Channels)
	}

	// Switch channel
	if err := wsConn.WriteJSON(IncomingAction{
		Action: "switch_channel",
		Name:   "architecture",
	}); err != nil {
		t.Fatalf("failed to switch channel: %v", err)
	}
	var statusAfterSwitch StatusResponse
	readWSJSON(t, wsConn, &statusAfterSwitch, 2*time.Second)
	if statusAfterSwitch.ActiveChannel != "architecture" {
		t.Fatalf("active channel = %s, want 'architecture'", statusAfterSwitch.ActiveChannel)
	}
}

// 3. Test joining spaces dynamically updates the status JSON
func TestJoinSpace(t *testing.T) {
	_, _, wsURL, _, cleanup := setupTestServer(t)
	defer cleanup()

	wsConn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial failed: %v", err)
	}
	defer wsConn.Close()

	// Drain initial status
	var initStatus StatusResponse
	readWSJSON(t, wsConn, &initStatus, 2*time.Second)

	// 1. Join space via action "join_space"
	extSpaceID := "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890"
	if err := wsConn.WriteJSON(IncomingAction{
		Action:  "join_space",
		SpaceID: extSpaceID,
		Name:    "External Matrix Room",
	}); err != nil {
		t.Fatalf("failed to send join_space: %v", err)
	}

	var statusAfterJoin StatusResponse
	readWSJSON(t, wsConn, &statusAfterJoin, 2*time.Second)

	if len(statusAfterJoin.Spaces) != 1 {
		t.Fatalf("expected 1 space after join_space, got %d", len(statusAfterJoin.Spaces))
	}
	if statusAfterJoin.Spaces[0].ID != extSpaceID {
		t.Fatalf("space ID mismatch: got %s, want %s", statusAfterJoin.Spaces[0].ID, extSpaceID)
	}
	if statusAfterJoin.Spaces[0].Name != "External Matrix Room" {
		t.Fatalf("space name mismatch: got %s, want 'External Matrix Room'", statusAfterJoin.Spaces[0].Name)
	}

	// 2. Join another space via "join_invite" with URI
	inviteURI := "wex://join/11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff?name=P2P_Cypherpunks"
	if err := wsConn.WriteJSON(IncomingAction{
		Action: "join_invite",
		URI:    inviteURI,
	}); err != nil {
		t.Fatalf("failed to send join_invite: %v", err)
	}

	var statusAfterInvite StatusResponse
	readWSJSON(t, wsConn, &statusAfterInvite, 2*time.Second)

	if len(statusAfterInvite.Spaces) != 2 {
		t.Fatalf("expected 2 spaces after join_invite, got %d", len(statusAfterInvite.Spaces))
	}
	foundP2P := false
	for _, s := range statusAfterInvite.Spaces {
		if s.Name == "P2P Cypherpunks" {
			foundP2P = true
			break
		}
	}
	if !foundP2P {
		t.Fatalf("space 'P2P Cypherpunks' not found in spaces: %v", statusAfterInvite.Spaces)
	}
}

// 4. Test profile updating (set_profile) saves to identity.json and persists
func TestProfileUpdatingAndPersistence(t *testing.T) {
	_, _, wsURL, dataDir, cleanup := setupTestServer(t)
	defer cleanup()

	wsConn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial failed: %v", err)
	}
	defer wsConn.Close()

	// Drain initial status
	var initStatus StatusResponse
	readWSJSON(t, wsConn, &initStatus, 2*time.Second)

	// Send set_profile action
	newNick := "Satoshi_Nakamoto"
	newStatus := "Building Sovereign Wex-Chat"
	if err := wsConn.WriteJSON(IncomingAction{
		Action:   "set_profile",
		Nickname: newNick,
		Status:   newStatus,
	}); err != nil {
		t.Fatalf("failed to send set_profile: %v", err)
	}

	// Receive broadcast status
	var statusAfterProfile StatusResponse
	readWSJSON(t, wsConn, &statusAfterProfile, 2*time.Second)

	if statusAfterProfile.Identity.Nickname != newNick {
		t.Fatalf("nickname = %s, want %s", statusAfterProfile.Identity.Nickname, newNick)
	}
	if statusAfterProfile.Identity.Status != newStatus {
		t.Fatalf("status = %s, want %s", statusAfterProfile.Identity.Status, newStatus)
	}

	// Verify persistence in identity.json on disk
	identPath := filepath.Join(dataDir, "identity.json")
	raw, err := os.ReadFile(identPath)
	if err != nil {
		t.Fatalf("failed to read identity.json at %s: %v", identPath, err)
	}

	var stored struct {
		Nickname string `json:"nickname"`
		Status   string `json:"status"`
		SeedHex  string `json:"seed_hex"`
	}
	if err := json.Unmarshal(raw, &stored); err != nil {
		t.Fatalf("failed to unmarshal identity.json: %v", err)
	}
	if stored.Nickname != newNick {
		t.Fatalf("stored nickname = %s, want %s", stored.Nickname, newNick)
	}
	if stored.Status != newStatus {
		t.Fatalf("stored status = %s, want %s", stored.Status, newStatus)
	}

	// Verify that reloading identity from disk recovers the updated profile
	reloaded, err := LoadOrCreateIdentity(dataDir, "FallbackUser")
	if err != nil {
		t.Fatalf("failed to reload identity: %v", err)
	}
	if reloaded.Nickname != newNick {
		t.Fatalf("reloaded nickname = %s, want %s", reloaded.Nickname, newNick)
	}
	if reloaded.Status != newStatus {
		t.Fatalf("reloaded status = %s, want %s", reloaded.Status, newStatus)
	}
}

// 5. Test Voice State transitions (join_voice, leave_voice)
func TestVoiceStateTransitions(t *testing.T) {
	_, _, wsURL, _, cleanup := setupTestServer(t)
	defer cleanup()

	wsConn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial failed: %v", err)
	}
	defer wsConn.Close()

	// Drain initial status
	var initStatus StatusResponse
	readWSJSON(t, wsConn, &initStatus, 2*time.Second)
	if initStatus.Voice.Connected {
		t.Fatalf("expected voice initially disconnected")
	}

	// Join voice
	if err := wsConn.WriteJSON(IncomingAction{
		Action:  "join_voice",
		Channel: "voice-hq",
	}); err != nil {
		t.Fatalf("failed to send join_voice: %v", err)
	}

	var voiceState map[string]interface{}
	readWSJSON(t, wsConn, &voiceState, 2*time.Second)

	if voiceState["type"] != "voice_state" {
		t.Fatalf("type = %v, want 'voice_state'", voiceState["type"])
	}
	if voiceState["connected"] != true {
		t.Fatalf("connected = %v, want true", voiceState["connected"])
	}
	if voiceState["channel"] != "voice-hq" {
		t.Fatalf("channel = %v, want 'voice-hq'", voiceState["channel"])
	}

	// Verify get_status reflects voice connection
	if err := wsConn.WriteJSON(IncomingAction{Action: "get_status"}); err != nil {
		t.Fatalf("failed to send get_status: %v", err)
	}
	var statusWithVoice StatusResponse
	readWSJSON(t, wsConn, &statusWithVoice, 2*time.Second)
	if !statusWithVoice.Voice.Connected {
		t.Fatalf("status.voice.connected = false, want true")
	}
	if statusWithVoice.Voice.Channel != "voice-hq" {
		t.Fatalf("status.voice.channel = %s, want 'voice-hq'", statusWithVoice.Voice.Channel)
	}

	// Leave voice
	if err := wsConn.WriteJSON(IncomingAction{Action: "leave_voice"}); err != nil {
		t.Fatalf("failed to send leave_voice: %v", err)
	}
	readWSJSON(t, wsConn, &voiceState, 2*time.Second)
	if voiceState["connected"] != false {
		t.Fatalf("connected = %v, want false after leave_voice", voiceState["connected"])
	}

	// Verify get_status reflects disconnection
	if err := wsConn.WriteJSON(IncomingAction{Action: "get_status"}); err != nil {
		t.Fatalf("failed to send get_status: %v", err)
	}
	var statusWithoutVoice StatusResponse
	readWSJSON(t, wsConn, &statusWithoutVoice, 2*time.Second)
	if statusWithoutVoice.Voice.Connected {
		t.Fatalf("status.voice.connected = true, want false")
	}
}

// 6. Test Multiple WebSocket Clients real-time broadcast synchronization
func TestMultipleClientsBroadcastSync(t *testing.T) {
	_, _, wsURL, _, cleanup := setupTestServer(t)
	defer cleanup()

	// Connect Client A
	connA, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial client A failed: %v", err)
	}
	defer connA.Close()

	// Connect Client B
	connB, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial client B failed: %v", err)
	}
	defer connB.Close()

	// Drain initial status on both
	var initA, initB StatusResponse
	readWSJSON(t, connA, &initA, 2*time.Second)
	readWSJSON(t, connB, &initB, 2*time.Second)

	// Client A creates a space
	if err := connA.WriteJSON(IncomingAction{
		Action: "create_space",
		Name:   "Synched Space",
	}); err != nil {
		t.Fatalf("client A create_space failed: %v", err)
	}

	// Both Client A and Client B should receive the broadcast status update
	var updateA, updateB StatusResponse
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		readWSJSON(t, connA, &updateA, 2*time.Second)
	}()
	go func() {
		defer wg.Done()
		readWSJSON(t, connB, &updateB, 2*time.Second)
	}()
	wg.Wait()

	if len(updateA.Spaces) != 1 || updateA.Spaces[0].Name != "Synched Space" {
		t.Fatalf("client A did not get correct space: %v", updateA.Spaces)
	}
	if len(updateB.Spaces) != 1 || updateB.Spaces[0].Name != "Synched Space" {
		t.Fatalf("client B did not get correct space: %v", updateB.Spaces)
	}
}

// 7. Concurrency / Race detector stress test
func TestServerConcurrencyStress(t *testing.T) {
	_, _, wsURL, _, cleanup := setupTestServer(t)
	defer cleanup()

	const clientCount = 5
	const iterations = 10

	var clients []*websocket.Conn
	for i := 0; i < clientCount; i++ {
		c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
		if err != nil {
			t.Fatalf("client %d dial failed: %v", i, err)
		}
		defer c.Close()
		clients = append(clients, c)
		// Read initial status
		var s StatusResponse
		readWSJSON(t, c, &s, 2*time.Second)
	}

	var wg sync.WaitGroup
	for idx, c := range clients {
		wg.Add(1)
		go func(clientIdx int, conn *websocket.Conn) {
			defer wg.Done()
			for j := 0; j < iterations; j++ {
				// Send actions
				_ = conn.WriteJSON(IncomingAction{
					Action:   "set_profile",
					Nickname: fmt.Sprintf("User_%d_%d", clientIdx, j),
					Status:   "Active",
				})
				_ = conn.WriteJSON(IncomingAction{
					Action: "get_status",
				})
				_ = conn.WriteJSON(IncomingAction{
					Action:  "join_voice",
					Channel: fmt.Sprintf("room-%d", clientIdx),
				})
				_ = conn.WriteJSON(IncomingAction{
					Action: "leave_voice",
				})

				// Drain a message
				_, _, _ = conn.ReadMessage()
			}
		}(idx, c)
	}

	wg.Wait()
}
