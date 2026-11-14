// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"sync"
	"testing"
	"time"
)

func eventRead(t testing.TB, c net.Conn, kind byte) Frame {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	f, e := ReadFrame(c)
	if e != nil || f.Type != kind {
		t.Fatalf("expected type %d, got %d/error %v", kind, f.Type, e)
	}
	return f
}
func eventJoin(t testing.TB, address, room, token string) (net.Conn, uint64) {
	t.Helper()
	c, e := net.Dial("tcp", address)
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { c.Close() })
	body, _ := json.Marshal(JoinRequest{Version: 1, Room: room, Password: testPassword, PeerEvents: true, Instance: token})
	if e = WriteFrame(c, Frame{Join, body}); e != nil {
		t.Fatal(e)
	}
	w := eventRead(t, c, Welcome)
	ack := eventRead(t, c, Capabilities)
	if string(ack.Payload) != "\x01" || len(w.Payload) != 8 {
		t.Fatal("bad capability admission")
	}
	return c, binary.BigEndian.Uint64(w.Payload)
}
func tag(t testing.TB, frame Frame) uint64 {
	t.Helper()
	if len(frame.Payload) != 8 {
		t.Fatal("bad membership size")
	}
	return binary.BigEndian.Uint64(frame.Payload)
}
func TestMembershipLegacyAndReconnectOrdering(t *testing.T) {
	s, address := launch(t)
	observer, observerID := eventJoin(t, address, "alpha", "00000000000000000000000000000001")
	legacy, legacyID := join(t, address, "alpha")
	if tag(t, eventRead(t, observer, PeerJoined)) != legacyID {
		t.Fatal("legacy arrival missing")
	}
	first, firstID := eventJoin(t, address, "alpha", "00000000000000000000000000000002")
	seen := map[uint64]bool{}
	for i := 0; i < 2; i++ {
		seen[tag(t, eventRead(t, first, PeerJoined))] = true
	}
	if !seen[observerID] || !seen[legacyID] {
		t.Fatal("initial peers missing")
	}
	if tag(t, eventRead(t, observer, PeerJoined)) != firstID {
		t.Fatal("first arrival missing")
	}
	s.mu.Lock()
	retired := s.peers[firstID]
	s.mu.Unlock()
	second, secondID := eventJoin(t, address, "alpha", "00000000000000000000000000000002")
	for i := 0; i < 2; i++ {
		eventRead(t, second, PeerJoined)
	}
	if tag(t, eventRead(t, observer, PeerLeft)) != firstID || tag(t, eventRead(t, observer, PeerJoined)) != secondID {
		t.Fatal("rejoin ordering wrong")
	}
	// A source retired under the lock cannot forward even before its socket cleanup ends.
	s.forward(retired, Frame{Packet, []byte("stale source must never forward")})
	if e := WriteFrame(second, Frame{Packet, []byte("new source")}); e != nil {
		t.Fatal(e)
	}
	packet := eventRead(t, observer, Packet)
	if binary.BigEndian.Uint64(packet.Payload[:8]) != secondID || string(packet.Payload[8:]) != "new source" {
		t.Fatal("retired source forwarded or new source missing")
	}
	// Legacy receives no notices, only the original source-tagged data stream.
	old := eventRead(t, legacy, Packet)
	if string(old.Payload[8:]) != "new source" {
		t.Fatal("legacy stream changed")
	}
	first.Close()
	if e := WriteFrame(second, Frame{Packet, []byte("still current")}); e != nil {
		t.Fatal(e)
	}
	next := eventRead(t, observer, Packet)
	if string(next.Payload[8:]) != "still current" {
		t.Fatal("old cleanup removed replacement")
	}
}
func TestMembershipChurnAndTokenIsolation(t *testing.T) {
	_, address := launch(t)
	observer, _ := eventJoin(t, address, "alpha", "10000000000000000000000000000000")
	var current uint64
	for i := 0; i < 48; i++ {
		c, id := eventJoin(t, address, "alpha", "20000000000000000000000000000000")
		eventRead(t, c, PeerJoined)
		if current != 0 && tag(t, eventRead(t, observer, PeerLeft)) != current {
			t.Fatal("wrong retired peer")
		}
		if tag(t, eventRead(t, observer, PeerJoined)) != id {
			t.Fatal("wrong admitted peer")
		}
		current = id
	}
	different, id := eventJoin(t, address, "alpha", "30000000000000000000000000000000")
	eventRead(t, different, PeerJoined)
	eventRead(t, different, PeerJoined)
	if tag(t, eventRead(t, observer, PeerJoined)) != id {
		t.Fatal("unrelated token retired active peer")
	}
	// Same token in a different authenticated room cannot retire the original room peer.
	beta, _ := eventJoin(t, address, "beta", "20000000000000000000000000000000")
	defer beta.Close()
	observer.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	if _, e := ReadFrame(observer); e == nil {
		t.Fatal("cross-room membership leaked")
	}
}
func TestConcurrentReconnectMembershipFIFO(t *testing.T) {
	s, address := launch(t)
	observer, _ := eventJoin(t, address, "alpha", "40000000000000000000000000000000")
	old, active := eventJoin(t, address, "alpha", "50000000000000000000000000000000")
	eventRead(t, old, PeerJoined)
	eventRead(t, observer, PeerJoined)
	var group sync.WaitGroup
	connections := make(chan net.Conn, 8)
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			c, e := net.Dial("tcp", address)
			if e != nil {
				return
			}
			connections <- c
			body, _ := json.Marshal(JoinRequest{Version: 1, Room: "alpha", Password: testPassword, PeerEvents: true, Instance: "50000000000000000000000000000000"})
			WriteFrame(c, Frame{Join, body})
			c.SetReadDeadline(time.Now().Add(2 * time.Second))
			ReadFrame(c)
		}()
	}
	group.Wait()
	close(connections)
	for c := range connections {
		defer c.Close()
	}
	for i := 0; i < 8; i++ {
		if tag(t, eventRead(t, observer, PeerLeft)) != active {
			t.Fatal("leave did not precede replacement")
		}
		active = tag(t, eventRead(t, observer, PeerJoined))
	}
	s.mu.Lock()
	current := s.peers[active]
	s.mu.Unlock()
	if current == nil {
		t.Fatal("deferred cleanup retired new peer")
	}
}

func TestPeerCannotForgeMembership(t *testing.T) {
	_, address := launch(t)
	observer, observerID := eventJoin(t, address, "alpha", "60000000000000000000000000000000")
	attacker, attackerID := eventJoin(t, address, "alpha", "70000000000000000000000000000000")
	eventRead(t, attacker, PeerJoined)
	eventRead(t, observer, PeerJoined)
	var fake [8]byte
	binary.BigEndian.PutUint64(fake[:], observerID)
	if err := WriteFrame(attacker, Frame{PeerLeft, fake[:]}); err != nil {
		t.Fatal(err)
	}
	if tag(t, eventRead(t, observer, PeerLeft)) != attackerID {
		t.Fatal("peer controlled membership retirement")
	}
}
func TestMembershipNeedsBoundedHandshakeSpace(t *testing.T) {
	s, _ := New(Config{Rooms: map[string]string{"alpha": testPassword}, QueueDepth: 1})
	server, client := net.Pipe()
	defer client.Close()
	done := make(chan struct{})
	go func() { defer close(done); s.ServeConn(server) }()
	body, _ := json.Marshal(JoinRequest{Version: 1, Room: "alpha", Password: testPassword, PeerEvents: true})
	client.SetDeadline(time.Now().Add(time.Second))
	if err := WriteFrame(client, Frame{Join, body}); err != nil {
		t.Fatal(err)
	}
	eventRead(t, client, Failure)
	client.Close()
	<-done
}
