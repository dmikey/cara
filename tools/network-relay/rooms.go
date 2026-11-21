// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"time"
	"unicode"
)

const (
	publicRoomCapacity = 4
	publicRoomLimit    = 64
	emptyRoomLifetime  = 2 * time.Minute
)

// GameIdentity describes the executable and version loaded by Cara. It is a
// compatibility check, not an authentication credential.
type GameIdentity struct {
	GameHash    string `json:"game_hash,omitempty"`
	GameVersion string `json:"game_version,omitempty"`
	TitleID     string `json:"title_id,omitempty"`
}

func (g GameIdentity) canonical() (GameIdentity, bool) {
	hash, hashErr := hex.DecodeString(g.GameHash)
	title, titleErr := hex.DecodeString(g.TitleID)
	if hashErr != nil || len(hash) != 32 || titleErr != nil || len(title) != 8 ||
		g.TitleID == "0000000000000000" || len(g.GameVersion) == 0 || len(g.GameVersion) > 96 {
		return GameIdentity{}, false
	}
	for _, c := range g.GameVersion {
		if (c < 'A' || c > 'Z') && (c < 'a' || c > 'z') && (c < '0' || c > '9') &&
			!strings.ContainsRune("._:+-", c) {
			return GameIdentity{}, false
		}
	}
	g.GameHash = hex.EncodeToString(hash)
	g.TitleID = hex.EncodeToString(title)
	return g, true
}

type publicRoom struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	GameIdentity
	Players  int `json:"players"`
	Capacity int `json:"capacity"`
	expires  time.Time
}

func (s *Server) publicOccupancyLocked() map[string]int {
	occupied := make(map[string]int)
	for _, peer := range s.peers {
		if _, ok := s.publicRooms[peer.room]; ok {
			occupied[peer.room]++
		}
	}
	return occupied
}

func (s *Server) reapPublicRoomsLocked(now time.Time, occupied map[string]int) {
	for id, room := range s.publicRooms {
		if occupied[id] == 0 && !room.expires.After(now) {
			delete(s.publicRooms, id)
		}
	}
}

func (s *Server) createPublicRoom(name string, game GameIdentity, now time.Time) (publicRoom, error) {
	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		return publicRoom{}, errors.New("Could not create a room right now")
	}
	id := "room-" + hex.EncodeToString(token[:])
	room := publicRoom{ID: id, Name: name, GameIdentity: game, Capacity: publicRoomCapacity,
		expires: now.Add(emptyRoomLifetime)}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.reapPublicRoomsLocked(now, s.publicOccupancyLocked())
	_, privateCollision := s.rooms[id]
	_, publicCollision := s.publicRooms[id]
	if s.closed || s.publicRooms == nil || len(s.publicRooms) >= publicRoomLimit ||
		privateCollision || publicCollision {
		return publicRoom{}, errors.New("Room directory is full or unavailable. Try again shortly.")
	}
	s.publicRooms[id] = room
	return room, nil
}

func (s *Server) browsePublicRooms(game GameIdentity, now time.Time) []publicRoom {
	s.mu.Lock()
	defer s.mu.Unlock()
	rooms := make([]publicRoom, 0)
	if s.closed {
		return rooms
	}
	occupied := s.publicOccupancyLocked()
	s.reapPublicRoomsLocked(now, occupied)
	for id, room := range s.publicRooms {
		if room.GameHash == game.GameHash && room.TitleID == game.TitleID {
			room.Players = occupied[id]
			rooms = append(rooms, room)
		}
	}
	sort.Slice(rooms, func(i, j int) bool {
		if rooms[i].Name != rooms[j].Name {
			return rooms[i].Name < rooms[j].Name
		}
		return rooms[i].ID < rooms[j].ID
	})
	return rooms
}

func (s *Server) roomDirectory(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	write := func(status int, value any) {
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(value)
	}
	reject := func(status int, message string) {
		write(status, map[string]string{"error": message})
	}
	if s.publicRooms == nil {
		reject(http.StatusNotFound, "Online rooms are unavailable on this relay")
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		reject(http.StatusMethodNotAllowed, "Room requests use POST")
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" {
		u, err := url.Parse(origin)
		if err != nil || u.Scheme != "https" || !strings.EqualFold(u.Host, r.Host) ||
			u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
			reject(http.StatusForbidden, "Invalid request origin")
			return
		}
	}
	media, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || media != "application/json" || r.URL.RawQuery != "" {
		reject(http.StatusBadRequest, "Send game details as JSON")
		return
	}
	var request struct {
		Name string `json:"name,omitempty"`
		GameIdentity
	}
	body := http.MaxBytesReader(w, r.Body, 2048)
	defer body.Close()
	decoder := json.NewDecoder(body)
	decoder.DisallowUnknownFields()
	if decoder.Decode(&request) != nil || decoder.Decode(new(any)) != io.EOF {
		reject(http.StatusBadRequest, "Invalid room request")
		return
	}
	game, ok := request.GameIdentity.canonical()
	if !ok {
		reject(http.StatusBadRequest, "A valid game hash, title and version are required")
		return
	}
	now := time.Now()
	if r.URL.Path == "/rooms/v1/browse" {
		write(http.StatusOK, map[string][]publicRoom{"rooms": s.browsePublicRooms(game, now)})
		return
	}
	name := strings.TrimSpace(request.Name)
	if len(name) == 0 || len(name) > 64 || strings.IndexFunc(name, func(c rune) bool {
		return !unicode.IsPrint(c)
	}) >= 0 {
		reject(http.StatusBadRequest, "Room name must contain 1–64 bytes of printable text")
		return
	}
	room, err := s.createPublicRoom(name, game, now)
	if err != nil {
		reject(http.StatusServiceUnavailable, err.Error())
		return
	}
	write(http.StatusOK, map[string]publicRoom{"room": room})
}
