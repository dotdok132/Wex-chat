package main

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"time"
)

const stunMagicCookie = 0x2112A442

var defaultSTUNServers = []string{
	"stun.l.google.com:19302",
	"stun1.l.google.com:19302",
	"stun2.l.google.com:19302",
	"stun.cloudflare.com:3478",
	"stun.matrix.org:3478",
}

// DiscoverExternalSTUN probes well-known STUN servers to discover public reflexive WAN IP:Port
func DiscoverExternalSTUN(timeout time.Duration) (string, error) {
	for _, srv := range defaultSTUNServers {
		ep, err := querySTUN(srv, timeout)
		if err == nil && ep != "" {
			return ep, nil
		}
	}
	return "", errors.New("stun discovery: all servers unreachable or timed out")
}

func querySTUN(serverAddr string, timeout time.Duration) (string, error) {
	conn, err := net.DialTimeout("udp4", serverAddr, timeout)
	if err != nil {
		return "", err
	}
	defer conn.Close()

	_ = conn.SetDeadline(time.Now().Add(timeout))

	// STUN Binding Request (20 bytes)
	req := make([]byte, 20)
	binary.BigEndian.PutUint16(req[0:2], 0x0001) // Binding Request
	binary.BigEndian.PutUint16(req[2:4], 0x0000) // Length
	binary.BigEndian.PutUint32(req[4:8], stunMagicCookie)
	if _, err := rand.Read(req[8:20]); err != nil {
		return "", err
	}

	if _, err := conn.Write(req); err != nil {
		return "", err
	}

	resp := make([]byte, 1024)
	n, err := conn.Read(resp)
	if err != nil || n < 20 {
		return "", errors.New("stun: invalid response length")
	}

	msgType := binary.BigEndian.Uint16(resp[0:2])
	if msgType != 0x0101 { // Binding Success Response
		return "", errors.New("stun: not a success response")
	}
	cookie := binary.BigEndian.Uint32(resp[4:8])
	if cookie != stunMagicCookie {
		return "", errors.New("stun: magic cookie mismatch")
	}

	// Parse attributes
	offset := 20
	for offset+4 <= n {
		attrType := binary.BigEndian.Uint16(resp[offset : offset+2])
		attrLen := int(binary.BigEndian.Uint16(resp[offset+2 : offset+4]))
		offset += 4

		if offset+attrLen > n {
			break
		}

		if attrType == 0x0020 && attrLen >= 8 { // XOR-MAPPED-ADDRESS
			family := resp[offset+1]
			if family == 0x01 { // IPv4
				xorPort := binary.BigEndian.Uint16(resp[offset+2 : offset+4])
				xorIP := binary.BigEndian.Uint32(resp[offset+4 : offset+8])

				port := xorPort ^ uint16(stunMagicCookie>>16)
				ipVal := xorIP ^ stunMagicCookie
				ip := net.IPv4(byte(ipVal>>24), byte(ipVal>>16), byte(ipVal>>8), byte(ipVal))

				return fmt.Sprintf("%s:%d", ip.String(), port), nil
			}
		} else if attrType == 0x0001 && attrLen >= 8 { // MAPPED-ADDRESS
			family := resp[offset+1]
			if family == 0x01 { // IPv4
				port := binary.BigEndian.Uint16(resp[offset+2 : offset+4])
				ip := net.IPv4(resp[offset+4], resp[offset+5], resp[offset+6], resp[offset+7])

				return fmt.Sprintf("%s:%d", ip.String(), port), nil
			}
		}

		// Align to 4-byte boundary
		offset += (attrLen + 3) &^ 3
	}

	return "", errors.New("stun: reflexive address attribute not found")
}

// DetectLocalLANIP returns the primary local network IPv4 address
func DetectLocalLANIP(defaultPort int) string {
	conn, err := net.Dial("udp4", "8.8.8.8:53")
	if err == nil {
		defer conn.Close()
		if localAddr, ok := conn.LocalAddr().(*net.UDPAddr); ok {
			return fmt.Sprintf("%s:%d", localAddr.IP.String(), defaultPort)
		}
	}
	return fmt.Sprintf("127.0.0.1:%d", defaultPort)
}
