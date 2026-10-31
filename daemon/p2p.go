package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/libp2p/go-libp2p"
	dht "github.com/libp2p/go-libp2p-kad-dht"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/p2p/discovery/mdns"
	"github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/relay"
	"github.com/multiformats/go-multiaddr"
)

const (
	ProtocolDM       = protocol.ID("/wex/dm/1.0.0")
	ProtocolBlob     = protocol.ID("/wex/blob/1.0.0")
	DiscoveryTopic   = "wex-v1:discovery"
	DefaultSpaceHex  = "0000000000000000000000000000000000000000000000000000000000000000"
)

type P2PNode struct {
	ctx           context.Context
	cancel        context.CancelFunc
	host          host.Host
	dht           *dht.IpfsDHT
	pubsub        *pubsub.PubSub
	relayService  *relay.Relay
	identity      *Identity
	storage       *Storage
	isSeed        bool
	listenPort    int

	mu            sync.RWMutex
	topics        map[string]*pubsub.Topic
	subs          map[string]*pubsub.Subscription
	knownPeers    map[peer.ID]*PeerInfo
	activeSpace   string
	activeChannel string

	onMessage     func(msg Message)
	onPeerEvent   func()
}

type mdnsNotifee struct {
	node *P2PNode
}

func (n *mdnsNotifee) HandlePeerFound(pi peer.AddrInfo) {
	if pi.ID == n.node.host.ID() {
		return
	}
	ctx, cancel := context.WithTimeout(n.node.ctx, 5*time.Second)
	defer cancel()
	if err := n.node.host.Connect(ctx, pi); err == nil {
		log.Printf("[+] mDNS discovered local LAN peer: %s", pi.ID.String())
	}
}

func NewP2PNode(ctx context.Context, ident *Identity, storage *Storage, port int, isSeed bool, bootstrapPeers []string) (*P2PNode, error) {
	nodeCtx, cancel := context.WithCancel(ctx)

	tcpAddr := fmt.Sprintf("/ip4/0.0.0.0/tcp/%d", port)
	quicAddr := fmt.Sprintf("/ip4/0.0.0.0/udp/%d/quic-v1", port)

	opts := []libp2p.Option{
		libp2p.Identity(ident.libp2pPriv),
		libp2p.ListenAddrStrings(tcpAddr, quicAddr),
		libp2p.NATPortMap(), // UPnP & NAT-PMP port mapping on home routers!
		libp2p.EnableNATService(),
		libp2p.EnableHolePunching(), // DCUtR WAN hole-punching!
	}

	h, err := libp2p.New(opts...)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("libp2p new host: %w", err)
	}

	var rService *relay.Relay
	if isSeed {
		rService, err = relay.New(h)
		if err != nil {
			log.Printf("[-] Warning: Failed to initialize relay service: %v", err)
		} else {
			log.Printf("[+] Circuit Relay v2 enabled on seed node")
		}
	}

	// Initialize Dual DHT (LAN + WAN Kademlia)
	kadDHT, err := dht.New(h, dht.Mode(dht.ModeAutoServer))
	if err != nil {
		log.Printf("[-] Warning: DHT init error: %v", err)
	}

	// Initialize GossipSub PubSub
	ps, err := pubsub.NewGossipSub(nodeCtx, h)
	if err != nil {
		h.Close()
		cancel()
		return nil, fmt.Errorf("pubsub new: %w", err)
	}

	node := &P2PNode{
		ctx:           nodeCtx,
		cancel:        cancel,
		host:          h,
		dht:           kadDHT,
		pubsub:        ps,
		relayService:  rService,
		identity:      ident,
		storage:       storage,
		isSeed:        isSeed,
		listenPort:    port,
		topics:        make(map[string]*pubsub.Topic),
		subs:          make(map[string]*pubsub.Subscription),
		knownPeers:    make(map[peer.ID]*PeerInfo),
		activeSpace:   DefaultSpaceHex,
		activeChannel: "general",
	}

	// Stream handler for Direct Messages
	h.SetStreamHandler(ProtocolDM, node.handleInboundDM)

	// Stream handler for Blob data
	h.SetStreamHandler(ProtocolBlob, node.handleInboundBlob)

	// mDNS local LAN discovery
	mdnsService := mdns.NewMdnsService(h, "wex-chat", &mdnsNotifee{node: node})
	_ = mdnsService.Start()

	// Connect to bootstrap peers in background
	go node.bootstrapSwarm(bootstrapPeers)

	// Subscribe to discovery topic
	go node.startDiscovery()

	// Subscribe to default channel
	_ = node.SubscribeChannel(DefaultSpaceHex, "general")

	return node, nil
}

func (node *P2PNode) SetCallbacks(onMsg func(Message), onPeer func()) {
	node.mu.Lock()
	defer node.mu.Unlock()
	node.onMessage = onMsg
	node.onPeerEvent = onPeer
}

func (node *P2PNode) bootstrapSwarm(customPeers []string) {
	// Connect to default public IPFS/libp2p bootstrap peers
	allBootstraps := make([]string, 0, len(dht.DefaultBootstrapPeers)+len(customPeers))
	for _, m := range dht.DefaultBootstrapPeers {
		allBootstraps = append(allBootstraps, m.String())
	}
	allBootstraps = append(allBootstraps, customPeers...)

	var wg sync.WaitGroup
	for _, addrStr := range allBootstraps {
		if addrStr == "" {
			continue
		}
		addr, err := multiaddr.NewMultiaddr(addrStr)
		if err != nil {
			continue
		}
		info, err := peer.AddrInfoFromP2pAddr(addr)
		if err != nil {
			continue
		}
		if info.ID == node.host.ID() {
			continue
		}

		wg.Add(1)
		go func(pi peer.AddrInfo) {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(node.ctx, 10*time.Second)
			defer cancel()
			if err := node.host.Connect(ctx, pi); err == nil {
				log.Printf("[+] Connected to bootstrap peer %s", pi.ID.String())
			}
		}(*info)
	}

	wg.Wait()

	if node.dht != nil {
		if err := node.dht.Bootstrap(node.ctx); err != nil {
			log.Printf("[-] DHT bootstrap warning: %v", err)
		}
	}
}

func (node *P2PNode) startDiscovery() {
	topic, err := node.pubsub.Join(DiscoveryTopic)
	if err != nil {
		log.Printf("[-] Failed to join discovery topic: %v", err)
		return
	}
	sub, err := topic.Subscribe()
	if err != nil {
		log.Printf("[-] Failed to subscribe to discovery topic: %v", err)
		return
	}

	// Inbound discovery loop
	go func() {
		for {
			msg, err := sub.Next(node.ctx)
			if err != nil {
				return
			}
			if msg.ReceivedFrom == node.host.ID() {
				continue
			}

			var beacon DiscoveryBeacon
			if err := json.Unmarshal(msg.Data, &beacon); err == nil && beacon.PeerID != "" {
				pid, err := peer.Decode(beacon.PeerID)
				if err == nil {
					node.mu.Lock()
					node.knownPeers[pid] = &PeerInfo{
						Nickname:  beacon.Nickname,
						PublicKey: beacon.PublicKey,
						PeerID:    beacon.PeerID,
						IP:        node.getPeerIP(pid),
						Port:      node.listenPort,
						Online:    true,
					}
					node.mu.Unlock()

					if node.onPeerEvent != nil {
						node.onPeerEvent()
					}
				}
			}
		}
	}()

	// Outbound beacon ticker (every 10 seconds)
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-node.ctx.Done():
			return
		case <-ticker.C:
			node.broadcastBeacon(topic)
		}
	}
}

func (node *P2PNode) broadcastBeacon(topic *pubsub.Topic) {
	node.identity.mu.RLock()
	nick := node.identity.Nickname
	status := node.identity.Status
	pubHex := node.identity.PubHex
	node.identity.mu.RUnlock()

	addrs := make([]string, 0)
	for _, a := range node.host.Addrs() {
		addrs = append(addrs, a.String())
	}

	b := DiscoveryBeacon{
		PeerID:    node.host.ID().String(),
		PublicKey: pubHex,
		Nickname:  nick,
		Status:    status,
		Addrs:     addrs,
	}

	data, err := json.Marshal(b)
	if err == nil {
		_ = topic.Publish(node.ctx, data)
	}
}

func (node *P2PNode) channelTopicName(spaceID, channel string) string {
	if spaceID == "" {
		spaceID = DefaultSpaceHex
	}
	if channel == "" {
		channel = "general"
	}
	return fmt.Sprintf("wex-v1:space:%s:%s", spaceID, channel)
}

func (node *P2PNode) SubscribeChannel(spaceID, channel string) error {
	tName := node.channelTopicName(spaceID, channel)

	node.mu.Lock()
	if _, exists := node.subs[tName]; exists {
		node.mu.Unlock()
		return nil
	}

	topic, err := node.pubsub.Join(tName)
	if err != nil {
		node.mu.Unlock()
		return err
	}
	sub, err := topic.Subscribe()
	if err != nil {
		node.mu.Unlock()
		return err
	}

	node.topics[tName] = topic
	node.subs[tName] = sub
	node.activeSpace = spaceID
	node.activeChannel = channel
	node.mu.Unlock()

	go func(s *pubsub.Subscription) {
		for {
			msg, err := s.Next(node.ctx)
			if err != nil {
				return
			}
			if msg.ReceivedFrom == node.host.ID() {
				continue
			}

			var chatMsg Message
			if err := json.Unmarshal(msg.Data, &chatMsg); err == nil {
				if chatMsg.Type == "" {
					chatMsg.Type = "message"
				}
				if node.storage.AddMessage(chatMsg) {
					if node.onMessage != nil {
						node.onMessage(chatMsg)
					}
				}
			}
		}
	}(sub)

	return nil
}

func (node *P2PNode) PublishMessage(spaceID, channel string, msg Message) error {
	tName := node.channelTopicName(spaceID, channel)

	node.mu.Lock()
	topic, exists := node.topics[tName]
	node.mu.Unlock()

	if !exists {
		if err := node.SubscribeChannel(spaceID, channel); err != nil {
			return err
		}
		node.mu.Lock()
		topic = node.topics[tName]
		node.mu.Unlock()
	}

	data, err := json.Marshal(msg)
	if err != nil {
		return err
	}

	return topic.Publish(node.ctx, data)
}

func (node *P2PNode) SendDirectMessage(targetPKHex string, msg Message) error {
	// Look up PeerID from known peers matching PK
	node.mu.RLock()
	var targetPID peer.ID
	for pid, pInfo := range node.knownPeers {
		if strings.EqualFold(pInfo.PublicKey, targetPKHex) {
			targetPID = pid
			break
		}
	}
	node.mu.RUnlock()

	// If not in known peers, check if targetPKHex is directly a libp2p PeerID
	if targetPID == "" {
		if pid, err := peer.Decode(targetPKHex); err == nil {
			targetPID = pid
		}
	}

	// Fallback to topic broadcast for direct messages
	dmTopicName := fmt.Sprintf("wex-v1:dm:%s", strings.ToLower(targetPKHex))
	node.mu.Lock()
	dmTopic, exists := node.topics[dmTopicName]
	if !exists {
		var err error
		dmTopic, err = node.pubsub.Join(dmTopicName)
		if err == nil {
			node.topics[dmTopicName] = dmTopic
		}
	}
	node.mu.Unlock()

	data, err := json.Marshal(msg)
	if err != nil {
		return err
	}

	if dmTopic != nil {
		_ = dmTopic.Publish(node.ctx, data)
	}

	// Also attempt direct stream if target peer is connected or resolved
	if targetPID != "" {
		go func() {
			ctx, cancel := context.WithTimeout(node.ctx, 5*time.Second)
			defer cancel()
			s, err := node.host.NewStream(ctx, targetPID, ProtocolDM)
			if err == nil {
				defer s.Close()
				lenBuf := make([]byte, 4)
				binary.BigEndian.PutUint32(lenBuf, uint32(len(data)))
				_, _ = s.Write(lenBuf)
				_, _ = s.Write(data)
			}
		}()
	}

	return nil
}

func (node *P2PNode) handleInboundDM(s network.Stream) {
	defer s.Close()

	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(s, lenBuf); err != nil {
		return
	}
	dataLen := binary.BigEndian.Uint32(lenBuf)
	if dataLen > 1024*1024 { // 1MB limit
		return
	}

	buf := make([]byte, dataLen)
	if _, err := io.ReadFull(s, buf); err != nil {
		return
	}

	var msg Message
	if err := json.Unmarshal(buf, &msg); err == nil {
		msg.Type = "message"
		msg.Encrypted = true
		if node.storage.AddMessage(msg) {
			if node.onMessage != nil {
				node.onMessage(msg)
			}
		}
	}
}

func (node *P2PNode) handleInboundBlob(s network.Stream) {
	defer s.Close()

	// Read 64 hex characters (32 bytes hash)
	hashBuf := make([]byte, 64)
	if _, err := io.ReadFull(s, hashBuf); err != nil {
		return
	}
	hashHex := string(hashBuf)

	data, err := node.storage.LoadBlob(hashHex)
	if err != nil {
		return
	}

	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(data)))
	_, _ = s.Write(lenBuf)
	_, _ = s.Write(data)
}

func (node *P2PNode) RequestBlob(targetPID peer.ID, hashHex string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(node.ctx, 15*time.Second)
	defer cancel()

	s, err := node.host.NewStream(ctx, targetPID, ProtocolBlob)
	if err != nil {
		return nil, err
	}
	defer s.Close()

	if _, err := s.Write([]byte(hashHex)); err != nil {
		return nil, err
	}

	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(s, lenBuf); err != nil {
		return nil, err
	}
	dataLen := binary.BigEndian.Uint32(lenBuf)
	if dataLen > 50*1024*1024 { // 50MB limit
		return nil, fmt.Errorf("blob too large: %d", dataLen)
	}

	data := make([]byte, dataLen)
	if _, err := io.ReadFull(s, data); err != nil {
		return nil, err
	}

	return data, nil
}

func (node *P2PNode) ConnectPeer(targetStr string) error {
	targetStr = strings.TrimSpace(targetStr)

	// 1. Full wex:// URI
	if strings.HasPrefix(targetStr, "wex://") {
		// Example: wex://join/<space_id>?peer=<multiaddr>&name=...
		targetStr = strings.TrimPrefix(targetStr, "wex://join/")
		targetStr = strings.TrimPrefix(targetStr, "wex://peer/")
		parts := strings.Split(targetStr, "?")
		if len(parts) > 1 {
			for _, param := range strings.Split(parts[1], "&") {
				if strings.HasPrefix(param, "peer=") {
					targetStr = strings.TrimPrefix(param, "peer=")
					break
				}
			}
		}
	}

	// 2. Direct Multiaddr (e.g. /ip4/.../tcp/.../p2p/...)
	if strings.HasPrefix(targetStr, "/") {
		ma, err := multiaddr.NewMultiaddr(targetStr)
		if err == nil {
			info, err := peer.AddrInfoFromP2pAddr(ma)
			if err == nil {
				ctx, cancel := context.WithTimeout(node.ctx, 10*time.Second)
				defer cancel()
				return node.host.Connect(ctx, *info)
			}
		}
	}

	// 3. Raw Peer ID (resolve via DHT)
	if pid, err := peer.Decode(targetStr); err == nil {
		ctx, cancel := context.WithTimeout(node.ctx, 15*time.Second)
		defer cancel()
		if node.dht != nil {
			info, err := node.dht.FindPeer(ctx, pid)
			if err == nil {
				return node.host.Connect(ctx, info)
			}
		}
	}

	// 4. IP:Port fallback
	if host, port, err := net.SplitHostPort(targetStr); err == nil {
		maStr := fmt.Sprintf("/ip4/%s/tcp/%s", host, port)
		if ma, err := multiaddr.NewMultiaddr(maStr); err == nil {
			log.Printf("[*] Probing raw multiaddr %s", ma.String())
		}
	}

	return fmt.Errorf("unable to resolve target: %s", targetStr)
}

func (node *P2PNode) getPeerIP(pid peer.ID) string {
	conns := node.host.Network().ConnsToPeer(pid)
	if len(conns) > 0 {
		return conns[0].RemoteMultiaddr().String()
	}
	return "unknown"
}

func (node *P2PNode) GetConnectedPeersList() []PeerInfo {
	node.mu.RLock()
	defer node.mu.RUnlock()

	connectedIDs := node.host.Network().Peers()
	result := make([]PeerInfo, 0, len(connectedIDs))

	for _, pid := range connectedIDs {
		ip := node.getPeerIP(pid)
		pInfo, known := node.knownPeers[pid]

		nick := "Peer " + pid.String()[:8]
		pubKey := ""
		if known {
			if pInfo.Nickname != "" {
				nick = pInfo.Nickname
			}
			pubKey = pInfo.PublicKey
		}

		result = append(result, PeerInfo{
			Nickname:  nick,
			PublicKey: pubKey,
			PeerID:    pid.String(),
			IP:        ip,
			Port:      node.listenPort,
			UseRelay:  strings.Contains(ip, "p2p-circuit"),
			Online:    true,
		})
	}

	return result
}

func (node *P2PNode) GetExternalMultiaddrs() []string {
	addrs := node.host.Addrs()
	res := make([]string, 0, len(addrs))
	for _, a := range addrs {
		res = append(res, a.String()+"/p2p/"+node.host.ID().String())
	}
	return res
}

func (node *P2PNode) Close() {
	node.cancel()
	_ = node.host.Close()
}
