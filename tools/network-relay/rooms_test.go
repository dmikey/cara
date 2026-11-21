// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func testGame() GameIdentity {
	return GameIdentity{GameHash: strings.Repeat("a", 64), GameVersion: "base:1", TitleID: "0004000000055f00"}
}

func roomPost(t *testing.T, handler http.Handler, path string, body any) *httptest.ResponseRecorder {
	t.Helper()
	raw, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest(http.MethodPost, "https://rooms.test/rooms/v1/"+path, bytes.NewReader(raw))
	req.Header.Set("Content-Type", "application/json")
	resp := httptest.NewRecorder()
	handler.ServeHTTP(resp, req)
	return resp
}

func startRoomServer(t *testing.T, enabled bool) (*Server, http.Handler) {
	t.Helper()
	server, err := New(Config{Rooms: map[string]string{"private": "private-password-1234"}, PublicRooms: enabled})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(server.Close)
	return server, WebSocketHandler(server, "rooms.test", 32)
}

func createRoom(t *testing.T, handler http.Handler, game GameIdentity, name string) publicRoom {
	t.Helper()
	response := roomPost(t, handler, "create", struct {
		Name string `json:"name"`
		GameIdentity
	}{name, game})
	var result struct{ Room publicRoom }
	if response.Code != http.StatusOK || json.Unmarshal(response.Body.Bytes(), &result) != nil {
		t.Fatalf("create status %d, %s", response.Code, response.Body.String())
	}
	return result.Room
}

func TestPublicRoomDirectoryMatchesHashWithoutHidingVersion(t *testing.T) {
	_, api := startRoomServer(t, true)
	game := testGame()
	room := createRoom(t, api, game, "Front Yard")
	if room.Name != "Front Yard" || room.GameIdentity != game || room.Capacity != 4 ||
		!strings.HasPrefix(room.ID, "room-") || room.Players != 0 {
		t.Fatalf("invalid created room: %+v", room)
	}
	differentVersion := game
	differentVersion.GameVersion = "update:2"
	createRoom(t, api, differentVersion, "Updated Yard")
	otherGame := game
	otherGame.GameHash = strings.Repeat("b", 64)
	createRoom(t, api, otherGame, "Different hash")
	otherGame = game
	otherGame.TitleID = "0004000000055f01"
	createRoom(t, api, otherGame, "Different title")
	response := roomPost(t, api, "browse", game)
	var list struct{ Rooms []publicRoom }
	if response.Code != http.StatusOK || json.Unmarshal(response.Body.Bytes(), &list) != nil ||
		len(list.Rooms) != 2 || list.Rooms[0].ID != room.ID || list.Rooms[1].GameVersion != "update:2" {
		t.Fatalf("unexpected room list: %s", response.Body.String())
	}
	if strings.Contains(response.Body.String(), "private-password") {
		t.Fatal("private rooms must stay out of the directory")
	}
}

func sendRoomJoin(t *testing.T, server *Server, request JoinRequest) (net.Conn, Frame) {
	t.Helper()
	client, conn := net.Pipe()
	go server.ServeConn(conn)
	t.Cleanup(func() { client.Close() })
	client.SetDeadline(time.Now().Add(3 * time.Second))
	raw, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	if err := WriteFrame(client, Frame{Type: Join, Payload: raw}); err != nil {
		t.Fatal(err)
	}
	frame, err := ReadFrame(client)
	if err != nil {
		t.Fatal(err)
	}
	return client, frame
}

func TestPublicRoomEnforcesExactGameAtJoin(t *testing.T) {
	server, api := startRoomServer(t, true)
	game := testGame()
	room := createRoom(t, api, game, "Luigi")
	join := func(identity GameIdentity) (net.Conn, Frame) {
		return sendRoomJoin(t, server, JoinRequest{Version: 1, Room: room.ID, PeerEvents: true, GameIdentity: identity})
	}
	for i, mismatch := range []GameIdentity{
		{},
		{GameHash: strings.Repeat("b", 64), GameVersion: game.GameVersion, TitleID: game.TitleID},
		{GameHash: game.GameHash, GameVersion: "update:2", TitleID: game.TitleID},
		{GameHash: game.GameHash, GameVersion: game.GameVersion, TitleID: "0004000000055f01"},
	} {
		t.Run(fmt.Sprintf("mismatch-%d", i), func(t *testing.T) {
			conn, frame := join(mismatch)
			defer conn.Close()
			if frame.Type != Failure || !strings.Contains(string(frame.Payload), "Game hash or version") {
				t.Fatalf("join mismatch accepted: %+v", frame)
			}
		})
	}
	first, welcome := join(game)
	if welcome.Type != Welcome {
		t.Fatalf("expected welcome, got %+v", welcome)
	}
	if frame, _ := ReadFrame(first); frame.Type != Capabilities {
		t.Fatalf("expected capabilities, got %+v", frame)
	}
	second, frame := join(game)
	if frame.Type != Welcome {
		t.Fatalf("second peer cannot join: %+v", frame)
	}
	if frame, _ := ReadFrame(second); frame.Type != Capabilities {
		t.Fatal("missing second capabilities")
	}
	if frame, _ := ReadFrame(second); frame.Type != PeerJoined {
		t.Fatal("missing existing member")
	}
	if frame, _ := ReadFrame(first); frame.Type != PeerJoined {
		t.Fatal("missing new member")
	}
	payload := []byte("original local-wireless traffic")
	if err := WriteFrame(first, Frame{Type: Packet, Payload: payload}); err != nil {
		t.Fatal(err)
	}
	frame, err := ReadFrame(second)
	if err != nil || frame.Type != Packet || len(frame.Payload) < 8 ||
		!bytes.Equal(frame.Payload[:8], welcome.Payload) || !bytes.Equal(frame.Payload[8:], payload) {
		t.Fatalf("packet relay mismatch: %+v %v", frame, err)
	}
	response := roomPost(t, api, "browse", game)
	var list struct{ Rooms []publicRoom }
	if json.Unmarshal(response.Body.Bytes(), &list) != nil || len(list.Rooms) != 1 ||
		list.Rooms[0].Players != 2 {
		t.Fatalf("room occupancy mismatch: %s", response.Body.String())
	}
}

func TestPublicRoomDirectoryDisabledInvalidAndBounded(t *testing.T) {
	_, disabled := startRoomServer(t, false)
	game := testGame()
	if response := roomPost(t, disabled, "browse", game); response.Code != http.StatusNotFound {
		t.Fatalf("disabled status %d", response.Code)
	}
	server, api := startRoomServer(t, true)
	if response := roomPost(t, api, "browse", map[string]string{"game_hash": game.GameHash}); response.Code != http.StatusBadRequest {
		t.Fatalf("invalid identity accepted: %d", response.Code)
	}
	now := time.Unix(1_000, 0)
	for i := 0; i < publicRoomLimit; i++ {
		if _, err := server.createPublicRoom("Bounded room", game, now); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := server.createPublicRoom("Over limit", game, now); err == nil {
		t.Fatal("unbounded room creation")
	}
	if rooms := server.browsePublicRooms(game, now.Add(emptyRoomLifetime)); len(rooms) != 0 {
		t.Fatalf("expired rooms remain: %d", len(rooms))
	}
}

func TestPublicRoomOverTLS(t *testing.T) {
	backend, err := New(Config{PublicRooms: true})
	if err != nil {
		t.Fatal(err)
	}
	host := httptest.NewUnstartedServer(nil)
	host.Config.Handler = WebSocketHandler(backend, host.Listener.Addr().String(), 32)
	host.TLS = &tls.Config{MinVersion: tls.VersionTLS13}
	host.StartTLS()
	t.Cleanup(func() { backend.Close(); host.Close() })
	client := host.Client()
	client.Timeout = 3 * time.Second
	game := testGame()
	body, err := json.Marshal(struct {
		Name string `json:"name"`
		GameIdentity
	}{"Gate", game})
	if err != nil {
		t.Fatal(err)
	}
	response, err := client.Post(host.URL+"/rooms/v1/create", "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	var creation struct{ Room publicRoom }
	err = json.NewDecoder(response.Body).Decode(&creation)
	response.Body.Close()
	if err != nil || response.StatusCode != http.StatusOK || response.TLS.Version != tls.VersionTLS13 ||
		creation.Room.GameIdentity != game {
		t.Fatalf("HTTPS create failed: %v, status %d", err, response.StatusCode)
	}

	join := func(identity GameIdentity, expected byte) net.Conn {
		t.Helper()
		socket, upgrade, err := websocketDial(t, host, client, nil)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { socket.CloseNow() })
		if upgrade.TLS.Version != tls.VersionTLS13 || len(upgrade.TLS.VerifiedChains) == 0 {
			t.Fatal("WebSocket did not verify TLS 1.3")
		}
		conn := websocket.NetConn(context.Background(), socket, websocket.MessageBinary)
		conn.SetDeadline(time.Now().Add(2 * time.Second))
		raw, err := json.Marshal(JoinRequest{Version: 1, Room: creation.Room.ID, GameIdentity: identity, PeerEvents: true})
		if err != nil {
			t.Fatal(err)
		}
		if err := WriteFrame(conn, Frame{Type: Join, Payload: raw}); err != nil {
			t.Fatal(err)
		}
		frame, err := ReadFrame(conn)
		if err != nil || frame.Type != expected {
			t.Fatalf("unexpected WSS join: %+v, %v", frame, err)
		}
		return conn
	}
	wrong := game
	wrong.GameVersion = "update:2"
	join(wrong, Failure)
	for i := 0; i < publicRoomCapacity; i++ {
		join(game, Welcome)
	}
	join(game, Failure)
}
