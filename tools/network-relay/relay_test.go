// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"sync"
	"testing"
	"time"
)

const testPassword = "local-test-password-1234"

func launch(t testing.TB) (*Server, string) {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	return launchListener(t, l)
}

func launchListener(t testing.TB, l net.Listener) (*Server, string) {
	t.Helper()
	s, err := New(Config{Rooms: map[string]string{"alpha": testPassword, "beta": testPassword}})
	if err != nil {
		l.Close()
		t.Fatal(err)
	}
	var workers sync.WaitGroup
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			workers.Add(1)
			go func() { defer workers.Done(); s.ServeConn(c) }()
		}
	}()
	t.Cleanup(func() { l.Close(); <-done; s.Close(); workers.Wait() })
	return s, l.Addr().String()
}

func join(t testing.TB, address, room string) (net.Conn, uint64) {
	t.Helper()
	c, err := net.Dial("tcp", address)
	if err != nil {
		t.Fatal(err)
	}
	return joinConn(t, c, room)
}

func joinConn(t testing.TB, c net.Conn, room string) (net.Conn, uint64) {
	t.Helper()
	t.Cleanup(func() { c.Close() })
	c.SetDeadline(time.Now().Add(3 * time.Second))
	payload, _ := json.Marshal(JoinRequest{Version: 1, Room: room, Password: testPassword})
	if err := WriteFrame(c, Frame{Join, payload}); err != nil {
		t.Fatal(err)
	}
	f, err := ReadFrame(c)
	if err != nil || f.Type != Welcome || len(f.Payload) != 8 {
		t.Fatalf("join failed: %v %v", f, err)
	}
	c.SetDeadline(time.Time{})
	return c, binary.BigEndian.Uint64(f.Payload)
}

func TestRoomsForwardAndIsolate(t *testing.T) {
	_, address := launch(t)
	a, id := join(t, address, "alpha")
	b, _ := join(t, address, "alpha")
	c, _ := join(t, address, "beta")
	payload := []byte{0, 1, 255, 9, 0, 7}
	if err := WriteFrame(a, Frame{Packet, payload}); err != nil {
		t.Fatal(err)
	}
	b.SetReadDeadline(time.Now().Add(time.Second))
	f, err := ReadFrame(b)
	if err != nil || f.Type != Packet || len(f.Payload) < 8 || binary.BigEndian.Uint64(f.Payload[:8]) != id || !bytes.Equal(f.Payload[8:], payload) {
		t.Fatalf("bad forwarded packet: %v %v", f, err)
	}
	for _, isolated := range []net.Conn{a, c} {
		isolated.SetReadDeadline(time.Now().Add(30 * time.Millisecond))
		if _, err := ReadFrame(isolated); err == nil {
			t.Fatal("packet echoed to source or leaked to other room")
		} else if e, ok := err.(net.Error); !ok || !e.Timeout() {
			t.Fatalf("expected timeout: %v", err)
		}
	}
}

func TestRejectedJoinAndMalformedFrames(t *testing.T) {
	s, address := launch(t)
	for _, input := range []Frame{
		{Join, []byte(`{"version":1,"room":"alpha","password":"wrong"}`)},
		{Join, []byte(`{"version":1,"room":"missing","password":"local-test-password-1234"}`)},
		{Join, []byte(`{"version":99,"room":"alpha","password":"local-test-password-1234"}`)},
		{Join, []byte(`{broken`)},
		{Packet, []byte("join first")},
	} {
		c, err := net.Dial("tcp", address)
		if err != nil {
			t.Fatal(err)
		}
		c.SetDeadline(time.Now().Add(time.Second))
		if err := WriteFrame(c, input); err != nil {
			t.Fatal(err)
		}
		f, err := ReadFrame(c)
		if e, ok := err.(net.Error); ok && e.Timeout() {
			t.Fatalf("rejected join stalled: %v", err)
		}
		if err == nil && f.Type != Failure {
			t.Fatalf("unexpected success: %v", f)
		}
		if err == nil {
			if _, err := ReadFrame(c); err == nil {
				t.Fatal("connection remained usable after rejection")
			}
		}
		c.Close()
	}
	s.mu.Lock()
	count := len(s.peers)
	s.mu.Unlock()
	if count != 0 {
		t.Fatalf("rejected peers registered: %d", count)
	}
	var header [5]byte
	header[0] = Packet
	binary.BigEndian.PutUint32(header[1:], MaxPayload+1)
	if _, err := ReadFrame(bytes.NewReader(header[:])); err == nil {
		t.Fatal("oversized frame accepted")
	}
	if _, err := ReadFrame(bytes.NewReader([]byte{3, 0, 0, 0, 2, 1})); err != io.ErrUnexpectedEOF {
		t.Fatalf("truncation: %v", err)
	}
}

func TestMalformedJoinedPacketsDisconnect(t *testing.T) {
	_, address := launch(t)
	for _, frame := range []Frame{{Type: Packet}, {Type: Packet, Payload: make([]byte, MaxPayload-7)}, {Type: 255}} {
		c, _ := join(t, address, "alpha")
		if err := WriteFrame(c, frame); err != nil {
			t.Fatal(err)
		}
		c.SetReadDeadline(time.Now().Add(time.Second))
		if _, err := ReadFrame(c); err != io.EOF {
			t.Fatalf("malformed packet did not close session: %v", err)
		}
	}
}

func FuzzReadFrame(f *testing.F) {
	f.Add([]byte{Packet, 0, 0, 0, 1, 42})
	f.Add([]byte{Packet, 255, 255, 255, 255})
	f.Fuzz(func(t *testing.T, data []byte) {
		frame, err := ReadFrame(bytes.NewReader(data))
		if err == nil && len(frame.Payload) > MaxPayload {
			t.Fatal("unbounded frame")
		}
	})
}

func TestLeaveAndServerClose(t *testing.T) {
	s, address := launch(t)
	a, _ := join(t, address, "alpha")
	b, _ := join(t, address, "alpha")
	if err := WriteFrame(a, Frame{Type: Leave}); err != nil {
		t.Fatal(err)
	}
	a.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := ReadFrame(a); err != io.EOF {
		t.Fatalf("leave did not close peer: %v", err)
	}
	s.Close()
	b.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := ReadFrame(b); err != io.EOF {
		t.Fatalf("shutdown did not close peer: %v", err)
	}
}

func TestBoundedQueueDisconnectsSlowPeer(t *testing.T) {
	s, err := New(Config{Rooms: map[string]string{"alpha": testPassword}, QueueDepth: 1})
	if err != nil {
		t.Fatal(err)
	}
	serverSide, clientSide := net.Pipe()
	defer serverSide.Close()
	defer clientSide.Close()
	source := &peer{id: 1, room: "alpha"}
	target := &peer{id: 2, room: "alpha", conn: serverSide, out: make(chan Frame, 1)}
	s.peers[2] = target
	s.forward(source, Frame{Packet, []byte("first")})
	s.forward(source, Frame{Packet, []byte("second")})
	if len(target.out) != 1 {
		t.Fatal("queue grew beyond bound")
	}
	clientSide.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := ReadFrame(clientSide); err != io.EOF {
		t.Fatalf("slow peer not disconnected: %v", err)
	}
}

type blockingCloseConn struct {
	net.Conn
	started chan struct{}
	release chan struct{}
	once    sync.Once
}

func (c *blockingCloseConn) Close() error {
	c.once.Do(func() { close(c.started) })
	<-c.release
	return c.Conn.Close()
}

func TestConnectionCloseNeverHoldsRoomLock(t *testing.T) {
	for _, operation := range []string{"overflow", "shutdown"} {
		t.Run(operation, func(t *testing.T) {
			s, _ := New(Config{Rooms: map[string]string{"alpha": testPassword, "beta": testPassword}, QueueDepth: 1})
			base, client := net.Pipe()
			defer client.Close()
			blocked := &blockingCloseConn{Conn: base, started: make(chan struct{}), release: make(chan struct{})}
			slow := &peer{id: 2, room: "alpha", conn: blocked, out: make(chan Frame, 1)}
			slow.out <- Frame{Type: Packet}
			s.peers[2] = slow
			s.connections[blocked] = struct{}{}
			other := &peer{id: 4, room: "beta", out: make(chan Frame, 1)}
			s.peers[4] = other
			done := make(chan struct{})
			go func() {
				defer close(done)
				if operation == "overflow" {
					s.forward(&peer{id: 1, room: "alpha"}, Frame{Type: Packet})
				} else {
					s.Close()
				}
			}()
			defer func() { close(blocked.release); <-done }()
			select {
			case <-blocked.started:
			case <-time.After(time.Second):
				t.Fatal("close did not start")
			}
			progress := make(chan struct{})
			go func() { s.forward(&peer{id: 3, room: "beta"}, Frame{Packet, []byte("other room")}); close(progress) }()
			select {
			case <-progress:
			case <-time.After(time.Second):
				t.Fatal("blocking close held global room lock")
			}
			if len(other.out) != 1 {
				t.Fatal("other room could not make progress")
			}
		})
	}
}

func TestLimitsAndInvalidConfig(t *testing.T) {
	for _, c := range []Config{{}, {Rooms: map[string]string{"x": "short"}}, {Rooms: map[string]string{"x": testPassword}, MaxPeers: -1}} {
		if _, err := New(c); err == nil {
			t.Fatal("invalid config accepted")
		}
	}
	s, err := New(Config{Rooms: map[string]string{"x": testPassword}, MaxPeers: 1})
	if err != nil {
		t.Fatal(err)
	}
	first, firstClient := net.Pipe()
	defer first.Close()
	defer firstClient.Close()
	s.connections[first] = struct{}{}
	second, secondClient := net.Pipe()
	defer secondClient.Close()
	s.ServeConn(second)
	if _, err := secondClient.Read(make([]byte, 1)); err != io.EOF {
		t.Fatalf("connection cap ignored: %v", err)
	}
}

func BenchmarkLoopbackForward(b *testing.B) {
	_, address := launch(b)
	a, _ := join(b, address, "alpha")
	c, _ := join(b, address, "alpha")
	payload := make([]byte, 1200)
	b.SetBytes(int64(len(payload)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if err := WriteFrame(a, Frame{Packet, payload}); err != nil {
			b.Fatal(err)
		}
		if _, err := ReadFrame(c); err != nil {
			b.Fatal(err)
		}
	}
}
