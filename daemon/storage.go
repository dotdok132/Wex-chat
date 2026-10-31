package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

type Storage struct {
	mu          sync.RWMutex
	dataDir     string
	history     []Message
	historyFile string
	blobDir     string
}

func NewStorage(dataDir string) (*Storage, error) {
	if err := os.MkdirAll(dataDir, 0700); err != nil {
		return nil, fmt.Errorf("create data dir: %w", err)
	}

	blobDir := filepath.Join(dataDir, "blobs")
	if err := os.MkdirAll(blobDir, 0700); err != nil {
		return nil, fmt.Errorf("create blobs dir: %w", err)
	}

	histFile := filepath.Join(dataDir, "history.json")
	s := &Storage{
		dataDir:     dataDir,
		historyFile: histFile,
		blobDir:     blobDir,
		history:     make([]Message, 0, 100),
	}

	// Load existing history if present
	if data, err := os.ReadFile(histFile); err == nil {
		var msgs []Message
		if err := json.Unmarshal(data, &msgs); err == nil {
			s.history = msgs
		}
	}

	return s, nil
}

func (s *Storage) AddMessage(msg Message) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	// Deduplicate by signature if present
	if msg.Signature != "" {
		for _, m := range s.history {
			if m.Signature == msg.Signature {
				return false
			}
		}
	}

	s.history = append(s.history, msg)
	if len(s.history) > 500 {
		s.history = s.history[len(s.history)-500:]
	}

	s.saveHistoryLocked()
	return true
}

func (s *Storage) GetHistory() []Message {
	s.mu.RLock()
	defer s.mu.RUnlock()

	res := make([]Message, len(s.history))
	copy(res, s.history)
	return res
}

func (s *Storage) EditMessage(targetSig, newContent string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	for i := range s.history {
		if s.history[i].Signature == targetSig {
			s.history[i].Content = newContent
			s.saveHistoryLocked()
			return true
		}
	}
	return false
}

func (s *Storage) DeleteMessage(targetSig string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	for i := range s.history {
		if s.history[i].Signature == targetSig {
			s.history = append(s.history[:i], s.history[i+1:]...)
			s.saveHistoryLocked()
			return true
		}
	}
	return false
}

func (s *Storage) ToggleReaction(targetSig, emoji string) string {
	s.mu.Lock()
	defer s.mu.Unlock()

	for i := range s.history {
		if s.history[i].Signature == targetSig {
			curr := strings.TrimSpace(s.history[i].Reactions)
			parts := make([]string, 0)
			if curr != "" {
				parts = strings.Fields(curr)
			}

			found := false
			newParts := make([]string, 0, len(parts)+1)
			for _, p := range parts {
				if p == emoji {
					found = true
				} else {
					newParts = append(newParts, p)
				}
			}
			if !found {
				newParts = append(newParts, emoji)
			}

			summary := strings.Join(newParts, " ")
			s.history[i].Reactions = summary
			s.saveHistoryLocked()
			return summary
		}
	}
	return ""
}

func (s *Storage) saveHistoryLocked() {
	data, err := json.Marshal(s.history)
	if err == nil {
		_ = os.WriteFile(s.historyFile, data, 0600)
	}
}

func (s *Storage) SaveBlob(data []byte) (string, error) {
	h := sha256.Sum256(data)
	hashHex := hex.EncodeToString(h[:])
	path := filepath.Join(s.blobDir, hashHex+".bin")
	if err := os.WriteFile(path, data, 0644); err != nil {
		return "", err
	}
	return hashHex, nil
}

func (s *Storage) LoadBlob(hashHex string) ([]byte, error) {
	path := filepath.Join(s.blobDir, hashHex+".bin")
	return os.ReadFile(path)
}
