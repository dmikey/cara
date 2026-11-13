// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"sync"
	"time"
)

// Protocol v1: a one-byte type and four-byte big-endian payload length.
// This transports adapter-owned opaque packets, not Nintendo Internet services.
const (
	Join       byte = 1
	Welcome    byte = 2
	Packet     byte = 3
	Leave      byte = 4
	Failure    byte = 5
	MaxPayload      = 4096
)

type Frame struct {
	Type    byte
	Payload []byte
}

func ReadFrame(r io.Reader) (Frame, error) {
	var header [5]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return Frame{}, err
	}
	size := binary.BigEndian.Uint32(header[1:])
	if size > MaxPayload {
		return Frame{}, errors.New("frame too large")
	}
	f := Frame{Type: header[0], Payload: make([]byte, size)}
	_, err := io.ReadFull(r, f.Payload)
	return f, err
}

func WriteFrame(w io.Writer, f Frame) error {
	if len(f.Payload) > MaxPayload {
		return errors.New("frame too large")
	}
	var header [5]byte
	header[0] = f.Type
	binary.BigEndian.PutUint32(header[1:], uint32(len(f.Payload)))
	for _, b := range [][]byte{header[:], f.Payload} {
		for len(b) > 0 {
			n, err := w.Write(b)
			if err != nil {
				return err
			}
			if n == 0 {
				return io.ErrShortWrite
			}
			b = b[n:]
		}
	}
	return nil
}

type JoinRequest struct {
	Version  int    `json:"version"`
	Room     string `json:"room"`
	Password string `json:"password"`
}

type Config struct {
	Rooms      map[string]string
	MaxPeers   int
	QueueDepth int
}

type peer struct {
	id   uint64
	room string
	conn net.Conn
	out  chan Frame
	done chan struct{}
}

type Server struct {
	mu                   sync.Mutex
	rooms                map[string][32]byte
	peers                map[uint64]*peer
	connections          map[net.Conn]struct{}
	nextID               uint64
	maxPeers, queueDepth int
	closed               bool
}

func New(config Config) (*Server, error) {
	if len(config.Rooms) == 0 {
		return nil, errors.New("at least one password-protected room is required")
	}
	rooms := make(map[string][32]byte)
	for room, password := range config.Rooms {
		if len(room) == 0 || len(room) > 64 || len(password) < 16 {
			return nil, errors.New("room names must be 1..64 bytes and passwords at least 16 bytes")
		}
		rooms[room] = sha256.Sum256([]byte(password))
	}
	if config.MaxPeers == 0 {
		config.MaxPeers = 32
	}
	if config.QueueDepth == 0 {
		config.QueueDepth = 64
	}
	if config.MaxPeers < 1 || config.MaxPeers > 1024 || config.QueueDepth < 1 || config.QueueDepth > 1024 {
		return nil, errors.New("peer and queue limits must be 1..1024")
	}
	return &Server{rooms: rooms, peers: make(map[uint64]*peer), connections: make(map[net.Conn]struct{}), maxPeers: config.MaxPeers, queueDepth: config.QueueDepth}, nil
}

// ServeConn takes ownership of conn. The caller supplies TLS for remote peers.
func (s *Server) ServeConn(conn net.Conn) {
	s.mu.Lock()
	if s.closed || len(s.connections) >= s.maxPeers {
		s.mu.Unlock()
		conn.Close()
		return
	}
	s.connections[conn] = struct{}{}
	s.mu.Unlock()
	defer func() { conn.Close(); s.mu.Lock(); delete(s.connections, conn); s.mu.Unlock() }()
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	first, err := ReadFrame(conn)
	if err != nil || first.Type != Join {
		return
	}
	var request JoinRequest
	if json.Unmarshal(first.Payload, &request) != nil || request.Version != 1 {
		return
	}
	provided := sha256.Sum256([]byte(request.Password))
	wanted, exists := s.rooms[request.Room]
	if !exists || subtle.ConstantTimeCompare(provided[:], wanted[:]) != 1 {
		WriteFrame(conn, Frame{Type: Failure, Payload: []byte("join rejected")})
		return
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.nextID++
	p := &peer{id: s.nextID, room: request.Room, conn: conn, out: make(chan Frame, s.queueDepth), done: make(chan struct{})}
	var id [8]byte
	binary.BigEndian.PutUint64(id[:], p.id)
	p.out <- Frame{Type: Welcome, Payload: id[:]}
	s.peers[p.id] = p
	s.mu.Unlock()
	conn.SetDeadline(time.Time{})
	defer func() { s.mu.Lock(); delete(s.peers, p.id); close(p.done); s.mu.Unlock() }()
	go writePeer(p)
	for {
		frame, err := ReadFrame(conn)
		if err != nil {
			return
		}
		switch frame.Type {
		case Packet:
			// The source tag is assigned by this relay and cannot be supplied by a peer.
			if len(frame.Payload) == 0 || len(frame.Payload) > MaxPayload-8 {
				return
			}
			payload := make([]byte, 8+len(frame.Payload))
			binary.BigEndian.PutUint64(payload, p.id)
			copy(payload[8:], frame.Payload)
			s.forward(p, Frame{Type: Packet, Payload: payload})
		case Leave:
			return
		default:
			return
		}
	}
}

func writePeer(p *peer) {
	for {
		select {
		case <-p.done:
			return
		case frame := <-p.out:
			p.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if WriteFrame(p.conn, frame) != nil {
				p.conn.Close()
				return
			}
		}
	}
}

func (s *Server) forward(source *peer, frame Frame) {
	s.mu.Lock()
	var stalled []net.Conn
	for _, target := range s.peers {
		if target.id == source.id || target.room != source.room {
			continue
		}
		select {
		case target.out <- frame:
		default:
			// Disconnect a stalled peer; never stall other rooms or grow a queue.
			stalled = append(stalled, target.conn)
		}
	}
	s.mu.Unlock()
	// A TLS close may perform I/O; never hold the room lock while closing.
	for _, conn := range stalled {
		conn.Close()
	}
}

func (s *Server) Close() {
	s.mu.Lock()
	s.closed = true
	connections := make([]net.Conn, 0, len(s.connections))
	for conn := range s.connections {
		connections = append(connections, conn)
	}
	s.mu.Unlock()
	for _, conn := range connections {
		conn.Close()
	}
}
