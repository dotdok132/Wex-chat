package main

// Message matches the JSON structure expected by web/wex_app.js
type Message struct {
	Type        string `json:"type"`
	Author      string `json:"author"`
	AuthorPK    string `json:"author_pk,omitempty"`
	PeerPK      string `json:"peer_pk,omitempty"`
	Channel     string `json:"channel"`
	Content     string `json:"content"`
	Verified    bool   `json:"verified"`
	Encrypted   bool   `json:"encrypted"`
	Signature   string `json:"signature,omitempty"`
	TimestampMS int64  `json:"timestamp_ms"`
	Reactions   string `json:"reactions,omitempty"`
	SpaceID     string `json:"space_id,omitempty"`
}

// Space matches the space structure expected by web/wex_app.js
type Space struct {
	ID       string   `json:"id"`
	Name     string   `json:"name"`
	Channels []string `json:"channels"`
}

// PeerInfo represents a discovered network peer
type PeerInfo struct {
	Nickname  string `json:"nickname"`
	PublicKey string `json:"public_key"`
	PeerID    string `json:"peer_id"`
	IP        string `json:"ip"`
	Port      int    `json:"port"`
	UseRelay  bool   `json:"use_relay"`
	Online    bool   `json:"online"`
}

// DHTNode represents a DHT routing node for telemetry display
type DHTNode struct {
	ID     string `json:"id"`
	IP     string `json:"ip"`
	Port   int    `json:"port"`
	Bucket int    `json:"bucket"`
}

// IdentityInfo matches the identity section of the status JSON
type IdentityInfo struct {
	Nickname         string `json:"nickname"`
	Status           string `json:"status"`
	PublicKey        string `json:"public_key"`
	X25519Key        string `json:"x25519_key"`
	PeerID           string `json:"peer_id"`
	UDPPort          int    `json:"udp_port"`
	DHTContacts      int    `json:"dht_contacts"`
	OfflineShards    int    `json:"offline_shards"`
	ExternalEndpoint string `json:"external_endpoint"`
	WANEndpoint      string `json:"wan_endpoint"`
	LANEndpoint      string `json:"lan_endpoint"`
	InviteURI        string `json:"invite_uri"`
}

// VoiceState represents the current voice channel connection status
type VoiceState struct {
	Connected bool   `json:"connected"`
	Channel   string `json:"channel"`
}

// StatusResponse is sent on get_status and periodically
type StatusResponse struct {
	Type          string       `json:"type"`
	Identity      IdentityInfo `json:"identity"`
	Spaces        []Space      `json:"spaces"`
	ActiveChannel string       `json:"active_channel"`
	Peers         []PeerInfo   `json:"peers"`
	DHTNodes      []DHTNode    `json:"dht_nodes"`
	History       []Message    `json:"history"`
	Voice         VoiceState   `json:"voice"`
}

// IncomingAction represents WebSocket requests from the frontend
type IncomingAction struct {
	Action      string `json:"action"`
	Target      string `json:"target,omitempty"`
	URI         string `json:"uri,omitempty"`
	PublicKey   string `json:"public_key,omitempty"`
	PK          string `json:"pk,omitempty"`
	TargetPK    string `json:"target_pk,omitempty"`
	PeerPK      string `json:"peer_pk,omitempty"`
	IP          string `json:"ip,omitempty"`
	Port        int    `json:"port,omitempty"`
	SpaceID     string `json:"space_id,omitempty"`
	Name        string `json:"name,omitempty"`
	Nickname    string `json:"nickname,omitempty"`
	Status      string `json:"status,omitempty"`
	SeedHex     string `json:"seed_hex,omitempty"`
	Text        string `json:"text,omitempty"`
	Content     string `json:"content,omitempty"`
	Channel     string `json:"channel,omitempty"`
	TargetSig   string `json:"target_sig,omitempty"`
	Emoji       string `json:"emoji,omitempty"`
	Filename    string `json:"filename,omitempty"`
	MimeType    string `json:"mime_type,omitempty"`
	DataBase64  string `json:"data_base64,omitempty"`
	Hash        string `json:"hash,omitempty"`
}

// DiscoveryBeacon is sent over pubsub to announce presence
type DiscoveryBeacon struct {
	PeerID    string `json:"peer_id"`
	PublicKey string `json:"public_key"`
	Nickname  string `json:"nickname"`
	Status    string `json:"status"`
	Addrs     []string `json:"addrs"`
}
