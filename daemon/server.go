package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

type Server struct {
	httpPort int
	webDir   string
	ident    *Identity
	storage  *Storage
	node     *P2PNode
	dataDir  string

	mu             sync.RWMutex
	clients        map[*websocket.Conn]*sync.Mutex
	activeSpaceID  string
	activeChannel  string
	spaces         []Space
	voiceConnected bool
	voiceChannel   string

	upgrader   websocket.Upgrader
	httpServer *http.Server
	listener   net.Listener
}

func NewServer(port int, webDir, dataDir string, ident *Identity, storage *Storage, node *P2PNode) *Server {
	// Look for web directory in common locations
	resolvedWebDir := webDir
	if _, err := os.Stat(filepath.Join(resolvedWebDir, "index.html")); err != nil {
		if _, err := os.Stat("web/index.html"); err == nil {
			resolvedWebDir = "web"
		} else if _, err := os.Stat("../web/index.html"); err == nil {
			resolvedWebDir = "../web"
		}
	}

	s := &Server{
		httpPort:      port,
		webDir:        resolvedWebDir,
		ident:         ident,
		storage:       storage,
		node:          node,
		dataDir:       dataDir,
		clients:       make(map[*websocket.Conn]*sync.Mutex),
		activeSpaceID:  "",
		activeChannel:  "",
		spaces:         []Space{},
		voiceConnected: false,
		voiceChannel:   "",
		upgrader: websocket.Upgrader{
			CheckOrigin: func(r *http.Request) bool { return true },
		},
	}

	// Register P2P callbacks if node is provided
	if node != nil {
		node.SetCallbacks(
			func(msg Message) {
				s.BroadcastJSON(msg)
			},
			func() {
				s.BroadcastJSON(s.buildStatus())
			},
		)
	}

	return s
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	// 1. Static web assets
	fileServer := http.FileServer(http.Dir(s.webDir))
	mux.Handle("/", http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Check if it's a WebSocket upgrade request on root
		if strings.ToLower(r.Header.Get("Upgrade")) == "websocket" {
			s.handleWS(w, r)
			return
		}
		fileServer.ServeHTTP(w, r)
	}))

	// 2. Explicit /ws endpoint
	mux.HandleFunc("/ws", s.handleWS)

	return mux
}

func (s *Server) Start() error {
	addr := fmt.Sprintf("0.0.0.0:%d", s.httpPort)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	return s.Serve(ln)
}

func (s *Server) Serve(ln net.Listener) error {
	s.mu.Lock()
	s.listener = ln
	if tcpAddr, ok := ln.Addr().(*net.TCPAddr); ok {
		s.httpPort = tcpAddr.Port
	}
	s.httpServer = &http.Server{
		Handler: s.Handler(),
	}
	s.mu.Unlock()

	log.Printf("[+] Wex Web UI & WebSocket daemon: http://%s", ln.Addr().String())
	err := s.httpServer.Serve(ln)
	if err == http.ErrServerClosed {
		return nil
	}
	return err
}

func (s *Server) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()

	for conn, mu := range s.clients {
		mu.Lock()
		_ = conn.Close()
		mu.Unlock()
	}
	s.clients = make(map[*websocket.Conn]*sync.Mutex)

	if s.httpServer != nil {
		return s.httpServer.Close()
	}
	if s.listener != nil {
		return s.listener.Close()
	}
	return nil
}

func (s *Server) Addr() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.listener != nil {
		return s.listener.Addr().String()
	}
	return fmt.Sprintf("127.0.0.1:%d", s.httpPort)
}

func (s *Server) sendConnJSON(conn *websocket.Conn, v interface{}) error {
	s.mu.RLock()
	connMu := s.clients[conn]
	s.mu.RUnlock()

	if connMu == nil {
		return fmt.Errorf("connection not registered")
	}

	connMu.Lock()
	defer connMu.Unlock()
	return conn.WriteJSON(v)
}

func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := s.upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}

	connMu := &sync.Mutex{}
	s.mu.Lock()
	s.clients[conn] = connMu
	s.mu.Unlock()

	defer func() {
		s.mu.Lock()
		delete(s.clients, conn)
		s.mu.Unlock()

		connMu.Lock()
		_ = conn.Close()
		connMu.Unlock()
	}()

	// Send initial status
	_ = s.sendConnJSON(conn, s.buildStatus())

	for {
		_, p, err := conn.ReadMessage()
		if err != nil {
			break
		}

		var action IncomingAction
		if err := json.Unmarshal(p, &action); err != nil {
			continue
		}

		s.handleAction(conn, action)
	}
}

func (s *Server) handleAction(conn *websocket.Conn, a IncomingAction) {
	switch a.Action {
	case "get_status":
		_ = s.sendConnJSON(conn, s.buildStatus())

	case "connect_peer", "add_contact", "join_invite":
		target := a.Target
		if target == "" {
			target = a.URI
		}
		if target == "" {
			target = a.PublicKey
		}
		if target == "" {
			target = a.PK
		}
		if target == "" && a.IP != "" {
			if a.Port > 0 {
				target = fmt.Sprintf("%s:%d", a.IP, a.Port)
			} else {
				target = a.IP
			}
		}

		if strings.HasPrefix(target, "wex://join/") {
			uriPart := strings.TrimPrefix(target, "wex://join/")
			parts := strings.Split(uriPart, "?")
			spID := parts[0]
			spName := "Joined Space"
			if len(parts) > 1 {
				for _, param := range strings.Split(parts[1], "&") {
					if strings.HasPrefix(param, "name=") {
						spName = strings.ReplaceAll(strings.TrimPrefix(param, "name="), "_", " ")
					}
				}
			}
			if spID != "" {
				s.mu.Lock()
				exists := false
				for _, sp := range s.spaces {
					if sp.ID == spID {
						exists = true
						break
					}
				}
				if !exists {
					s.spaces = append(s.spaces, Space{
						ID:       spID,
						Name:     spName,
						Channels: []string{"general", "dev", "voice-lounge"},
					})
				}
				s.activeSpaceID = spID
				s.activeChannel = "general"
				s.mu.Unlock()

				if s.node != nil {
					_ = s.node.SubscribeChannel(spID, "general")
				}
				s.BroadcastJSON(s.buildStatus())
			}
		}

		if target != "" && s.node != nil {
			go func() {
				if err := s.node.ConnectPeer(target); err == nil {
					s.BroadcastJSON(s.buildStatus())
				}
			}()
		}

	case "join_space":
		if a.SpaceID != "" {
			s.mu.Lock()
			exists := false
			for _, sp := range s.spaces {
				if sp.ID == a.SpaceID {
					exists = true
					break
				}
			}
			if !exists {
				spName := a.Name
				if spName == "" {
					spName = "Joined Space"
				}
				s.spaces = append(s.spaces, Space{
					ID:       a.SpaceID,
					Name:     spName,
					Channels: []string{"general", "dev", "voice-lounge"},
				})
			}
			s.activeSpaceID = a.SpaceID
			s.activeChannel = "general"
			s.mu.Unlock()

			if s.node != nil {
				_ = s.node.SubscribeChannel(a.SpaceID, "general")
			}
			s.BroadcastJSON(s.buildStatus())
		}

	case "switch_space":
		if a.SpaceID != "" {
			s.mu.Lock()
			s.activeSpaceID = a.SpaceID
			s.mu.Unlock()
			if s.node != nil {
				_ = s.node.SubscribeChannel(s.activeSpaceID, s.activeChannel)
			}
			_ = s.sendConnJSON(conn, s.buildStatus())
		}

	case "create_space":
		if a.Name != "" {
			s.mu.Lock()
			h := sha256.Sum256([]byte(fmt.Sprintf("%s:%d", a.Name, time.Now().UnixNano())))
			spID := hex.EncodeToString(h[:])
			newSp := Space{
				ID:       spID,
				Name:     a.Name,
				Channels: []string{"general", "dev", "voice-lounge"},
			}
			s.spaces = append(s.spaces, newSp)
			s.activeSpaceID = spID
			s.activeChannel = "general"
			s.mu.Unlock()

			if s.node != nil {
				_ = s.node.SubscribeChannel(spID, "general")
			}
			s.BroadcastJSON(s.buildStatus())
		}

	case "create_channel":
		if a.Name != "" {
			s.mu.Lock()
			for i := range s.spaces {
				if s.spaces[i].ID == s.activeSpaceID {
					exists := false
					for _, ch := range s.spaces[i].Channels {
						if ch == a.Name {
							exists = true
							break
						}
					}
					if !exists {
						s.spaces[i].Channels = append(s.spaces[i].Channels, a.Name)
					}
					break
				}
			}
			s.mu.Unlock()
			if s.node != nil {
				_ = s.node.SubscribeChannel(s.activeSpaceID, a.Name)
			}
			s.BroadcastJSON(s.buildStatus())
		}

	case "switch_channel":
		if a.Name != "" {
			s.mu.Lock()
			s.activeChannel = a.Name
			s.mu.Unlock()
			if s.node != nil {
				_ = s.node.SubscribeChannel(s.activeSpaceID, s.activeChannel)
			}
			_ = s.sendConnJSON(conn, s.buildStatus())
		}

	case "set_profile":
		s.ident.SetProfile(a.Nickname, a.Status, s.dataDir)
		s.BroadcastJSON(s.buildStatus())

	case "export_secret":
		_ = s.sendConnJSON(conn, map[string]interface{}{
			"type":       "secret_exported",
			"seed_hex":   s.ident.SeedHex,
			"public_key": s.ident.PubHex,
		})

	case "import_secret":
		seedHex := a.SeedHex
		if len(seedHex) == 64 {
			if seedBytes, err := hex.DecodeString(seedHex); err == nil && len(seedBytes) == 32 {
				nick, status := s.ident.GetProfile()
				newIdent, err := NewIdentityFromSeed(seedBytes, nick, status)
				if err == nil {
					s.mu.Lock()
					s.ident = newIdent
					s.mu.Unlock()
					_ = newIdent.Save(s.dataDir)
					s.BroadcastJSON(s.buildStatus())
				}
			}
		}

	case "send_message":
		content := a.Text
		if content == "" {
			content = a.Content
		}
		ch := a.Channel
		if ch == "" {
			s.mu.RLock()
			ch = s.activeChannel
			s.mu.RUnlock()
		}
		if content != "" {
			nick, _ := s.ident.GetProfile()
			ts := time.Now().UnixMilli()
			signPayload := []byte(fmt.Sprintf("%s:%s:%d:%s", nick, ch, ts, content))
			sig := s.ident.Sign(signPayload)

			s.mu.RLock()
			actSpace := s.activeSpaceID
			s.mu.RUnlock()

			msg := Message{
				Type:        "message",
				Author:      nick,
				AuthorPK:    s.ident.PubHex,
				Channel:     ch,
				Content:     content,
				Verified:    true,
				Encrypted:   false,
				Signature:   hex.EncodeToString(sig),
				TimestampMS: ts,
				SpaceID:     actSpace,
			}

			s.storage.AddMessage(msg)
			s.BroadcastJSON(msg)
			if s.node != nil {
				_ = s.node.PublishMessage(actSpace, ch, msg)
			}
		}

	case "send_dm":
		targetPK := a.TargetPK
		if targetPK == "" {
			targetPK = a.PeerPK
		}
		content := a.Text
		if content == "" {
			content = a.Content
		}
		if targetPK != "" && content != "" {
			nick, _ := s.ident.GetProfile()
			ts := time.Now().UnixMilli()
			dmChannel := fmt.Sprintf("dm:%s", strings.ToLower(targetPK[:16]))
			signPayload := []byte(fmt.Sprintf("%s:%s:%d:%s", nick, dmChannel, ts, content))
			sig := s.ident.Sign(signPayload)

			msg := Message{
				Type:        "message",
				Author:      nick,
				AuthorPK:    s.ident.PubHex,
				PeerPK:      targetPK,
				Channel:     dmChannel,
				Content:     content,
				Verified:    true,
				Encrypted:   true,
				Signature:   hex.EncodeToString(sig),
				TimestampMS: ts,
			}

			s.storage.AddMessage(msg)
			s.BroadcastJSON(msg)
			if s.node != nil {
				_ = s.node.SendDirectMessage(targetPK, msg)
			}
		}

	case "upload_blob":
		if a.DataBase64 != "" {
			data, err := base64.StdEncoding.DecodeString(a.DataBase64)
			if err == nil {
				hashHex, err := s.storage.SaveBlob(data)
				if err == nil {
					fn := a.Filename
					if fn == "" {
						fn = "attachment.bin"
					}
					mt := a.MimeType
					if mt == "" {
						mt = "application/octet-stream"
					}

					attachMsg := fmt.Sprintf("[attachment:%s:%s:%d:%s]", hashHex, fn, len(data), mt)
					nick, _ := s.ident.GetProfile()
					ts := time.Now().UnixMilli()
					s.mu.RLock()
					actCh := s.activeChannel
					actSp := s.activeSpaceID
					s.mu.RUnlock()

					signPayload := []byte(fmt.Sprintf("%s:%s:%d:%s", nick, actCh, ts, attachMsg))
					sig := s.ident.Sign(signPayload)

					msg := Message{
						Type:        "message",
						Author:      nick,
						AuthorPK:    s.ident.PubHex,
						Channel:     actCh,
						Content:     attachMsg,
						Verified:    true,
						Signature:   hex.EncodeToString(sig),
						TimestampMS: ts,
						SpaceID:     actSp,
					}

					s.storage.AddMessage(msg)
					s.BroadcastJSON(msg)
					if s.node != nil {
						_ = s.node.PublishMessage(actSp, actCh, msg)
					}

					_ = s.sendConnJSON(conn, map[string]interface{}{
						"type":     "blob_uploaded",
						"hash":     hashHex,
						"filename": fn,
						"size":     len(data),
					})
				}
			}
		}

	case "get_blob":
		if a.Hash != "" {
			data, err := s.storage.LoadBlob(a.Hash)
			if err == nil {
				_ = s.sendConnJSON(conn, map[string]interface{}{
					"type":        "blob_data",
					"hash":        a.Hash,
					"data_base64": base64.StdEncoding.EncodeToString(data),
					"size":        len(data),
				})
			}
		}

	case "send_reaction":
		if a.TargetSig != "" && a.Emoji != "" {
			summary := s.storage.ToggleReaction(a.TargetSig, a.Emoji)
			s.BroadcastJSON(map[string]interface{}{
				"type":       "reaction_updated",
				"target_sig": a.TargetSig,
				"summary":    summary,
			})
		}

	case "edit_message":
		if a.TargetSig != "" && a.Text != "" {
			if s.storage.EditMessage(a.TargetSig, a.Text) {
				s.BroadcastJSON(map[string]interface{}{
					"type":       "message_edited",
					"target_sig": a.TargetSig,
					"content":    a.Text,
				})
			}
		}

	case "delete_message":
		if a.TargetSig != "" {
			if s.storage.DeleteMessage(a.TargetSig) {
				s.BroadcastJSON(map[string]interface{}{
					"type":       "message_deleted",
					"target_sig": a.TargetSig,
				})
			}
		}

	case "join_voice":
		ch := a.Channel
		if ch == "" {
			ch = "voice-lounge"
		}
		s.mu.Lock()
		s.voiceConnected = true
		s.voiceChannel = ch
		s.mu.Unlock()
		s.BroadcastJSON(map[string]interface{}{
			"type":      "voice_state",
			"connected": true,
			"channel":   ch,
		})

	case "leave_voice":
		s.mu.Lock()
		s.voiceConnected = false
		s.voiceChannel = ""
		s.mu.Unlock()
		s.BroadcastJSON(map[string]interface{}{
			"type":      "voice_state",
			"connected": false,
			"channel":   "",
		})
	}
}

func (s *Server) buildStatus() StatusResponse {
	s.mu.RLock()
	activeSpace := s.activeSpaceID
	activeChannel := s.activeChannel
	spacesCopy := make([]Space, len(s.spaces))
	copy(spacesCopy, s.spaces)
	vConn := s.voiceConnected
	vCh := s.voiceChannel
	ident := s.ident
	s.mu.RUnlock()

	var peers []PeerInfo
	var extEndpoint string
	var udpPort int
	if s.node != nil {
		peers = s.node.GetConnectedPeersList()
		externalAddrs := s.node.GetExternalMultiaddrs()
		if len(externalAddrs) > 0 {
			extEndpoint = externalAddrs[0]
		}
		udpPort = s.node.listenPort
	}
	if peers == nil {
		peers = []PeerInfo{}
	}

	nick, status := ident.GetProfile()

	spacePart := activeSpace
	if spacePart == "" {
		spacePart = "direct"
	}
	inviteURI := fmt.Sprintf("wex://join/%s?peer=%s&pk=%s&name=%s",
		spacePart, extEndpoint, ident.PubHex, "WexSpace")

	return StatusResponse{
		Type: "status",
		Identity: IdentityInfo{
			Nickname:         nick,
			Status:           status,
			PublicKey:        ident.PubHex,
			X25519Key:        ident.XPubHex,
			PeerID:           ident.peerID.String(),
			UDPPort:          udpPort,
			DHTContacts:      len(peers),
			OfflineShards:    0,
			ExternalEndpoint: extEndpoint,
			InviteURI:        inviteURI,
		},
		Spaces:        spacesCopy,
		ActiveChannel: activeChannel,
		Peers:         peers,
		DHTNodes:      []DHTNode{},
		History:       s.storage.GetHistory(),
		Voice: VoiceState{
			Connected: vConn,
			Channel:   vCh,
		},
	}
}

func (s *Server) BroadcastJSON(v interface{}) {
	data, err := json.Marshal(v)
	if err != nil {
		return
	}

	s.mu.RLock()
	type target struct {
		conn *websocket.Conn
		mu   *sync.Mutex
	}
	targets := make([]target, 0, len(s.clients))
	for conn, mu := range s.clients {
		targets = append(targets, target{conn: conn, mu: mu})
	}
	s.mu.RUnlock()

	for _, t := range targets {
		t.mu.Lock()
		_ = t.conn.WriteMessage(websocket.TextMessage, data)
		t.mu.Unlock()
	}
}
