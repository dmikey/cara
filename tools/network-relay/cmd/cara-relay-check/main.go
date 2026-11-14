// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// Bounded remote transport verification; never prints room credentials.
package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"time"

	relay "cara.network/relay"
	"github.com/coder/websocket"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "relay verification failed:", err)
		os.Exit(1)
	}
}
func run() error {
	endpoint := flag.String("endpoint", "", "trusted wss:// endpoint")
	roomsFile := flag.String("rooms-file", "", "private local JSON credentials, never printed")
	roomA := flag.String("room-a", "deploy-probe-a", "first configured test room")
	roomB := flag.String("room-b", "deploy-probe-b", "isolated configured test room")
	flag.Parse()
	u, err := url.Parse(*endpoint)
	if err != nil || u.Scheme != "wss" || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return fmt.Errorf("endpoint must be wss with no credentials, query or fragment")
	}
	file, err := os.Open(*roomsFile)
	if err != nil {
		return fmt.Errorf("private credentials unavailable")
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > 65536 {
		return fmt.Errorf("credential file must be private and bounded")
	}
	var rooms map[string]string
	if json.NewDecoder(file).Decode(&rooms) != nil || len(rooms[*roomA]) < 16 || len(rooms[*roomB]) < 16 {
		return fmt.Errorf("two private test rooms required")
	}
	client := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{MinVersion: tls.VersionTLS13}}, Timeout: 10 * time.Second}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	dial := func(room, password string) (*websocket.Conn, net.Conn, relay.Frame, error) {
		ws, response, err := websocket.Dial(ctx, *endpoint, &websocket.DialOptions{HTTPClient: client})
		if err != nil {
			return nil, nil, relay.Frame{}, fmt.Errorf("trusted WebSocket connection failed")
		}
		if response.TLS == nil || response.TLS.Version != tls.VersionTLS13 || len(response.TLS.VerifiedChains) == 0 {
			ws.CloseNow()
			return nil, nil, relay.Frame{}, fmt.Errorf("verified TLS1.3 not negotiated")
		}
		conn := websocket.NetConn(ctx, ws, websocket.MessageBinary)
		conn.SetDeadline(time.Now().Add(5 * time.Second))
		join, _ := json.Marshal(relay.JoinRequest{Version: 1, Room: room, Password: password})
		if relay.WriteFrame(conn, relay.Frame{Type: relay.Join, Payload: join}) != nil {
			ws.CloseNow()
			return nil, nil, relay.Frame{}, fmt.Errorf("join write failed")
		}
		frame, err := relay.ReadFrame(conn)
		if err != nil {
			ws.CloseNow()
			return nil, nil, relay.Frame{}, fmt.Errorf("join response failed")
		}
		return ws, conn, frame, nil
	}
	a, ac, aw, err := dial(*roomA, rooms[*roomA])
	if err != nil {
		return err
	}
	defer a.CloseNow()
	b, bc, bw, err := dial(*roomA, rooms[*roomA])
	if err != nil {
		return err
	}
	defer b.CloseNow()
	isolated, ic, iw, err := dial(*roomB, rooms[*roomB])
	if err != nil {
		return err
	}
	defer isolated.CloseNow()
	if aw.Type != relay.Welcome || bw.Type != relay.Welcome || iw.Type != relay.Welcome || len(aw.Payload) != 8 || len(bw.Payload) != 8 {
		return fmt.Errorf("private admission failed")
	}
	payload := make([]byte, relay.MaxPayload-8)
	for i := range payload {
		payload[i] = byte(i*37 + 255)
	}
	if relay.WriteFrame(ac, relay.Frame{Type: relay.Packet, Payload: payload}) != nil {
		return fmt.Errorf("packet write failed")
	}
	got, err := relay.ReadFrame(bc)
	if err != nil || got.Type != relay.Packet || len(got.Payload) != len(payload)+8 || string(got.Payload[8:]) != string(payload) || binary.BigEndian.Uint64(got.Payload[:8]) != binary.BigEndian.Uint64(aw.Payload) {
		return fmt.Errorf("binary payload/source verification failed")
	}
	bad, _, answer, err := dial(*roomA, "deliberately-invalid-password-for-probe")
	if err != nil {
		return err
	}
	bad.CloseNow()
	if answer.Type != relay.Failure {
		return fmt.Errorf("invalid password accepted")
	}
	ic.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
	if _, err := relay.ReadFrame(ic); err == nil {
		return fmt.Errorf("cross-room packet leak")
	}
	ac.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
	if _, err := relay.ReadFrame(ac); err == nil {
		return fmt.Errorf("sender received self echo")
	}
	for _, tlsConfig := range []*tls.Config{{MinVersion: tls.VersionTLS13, ServerName: "invalid-hostname.invalid"}, {MinVersion: tls.VersionTLS13, RootCAs: x509.NewCertPool()}} {
		wrong := &http.Client{Transport: &http.Transport{TLSClientConfig: tlsConfig}, Timeout: 5 * time.Second}
		ws, _, err := websocket.Dial(ctx, *endpoint, &websocket.DialOptions{HTTPClient: wrong})
		if err == nil {
			ws.CloseNow()
			return fmt.Errorf("invalid hostname or trust accepted")
		}
	}
	healthURL := *u
	healthURL.Scheme = "https"
	healthURL.Path = "/healthz"
	response, err := client.Get(healthURL.String())
	if err != nil {
		return fmt.Errorf("public health failed")
	}
	defer response.Body.Close()
	var health struct {
		Service, Status, Transport string
		Protocol                   int
	}
	if response.StatusCode != 200 || json.NewDecoder(response.Body).Decode(&health) != nil || health.Service != "cara-relay" || health.Status != "ok" || health.Transport != "websocket" || health.Protocol != 1 {
		return fmt.Errorf("public health response mismatch")
	}
	fmt.Println("PASS: trusted hostname TLS1.3, three private peers, exact 4088-byte forwarding/source tag, no echo, room isolation, wrong-password rejection, wrong-hostname/untrusted-CA rejection, public health")
	return nil
}
