package relay

import (
	"context"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func websocketFixture(t *testing.T, cap int) (*Server, *httptest.Server, *http.Client) {
	t.Helper()
	backend, err := New(Config{Rooms: map[string]string{"a": "test-room-password-aaaa", "b": "test-room-password-bbbb"}, MaxPeers: cap, QueueDepth: 4})
	if err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewUnstartedServer(nil)
	httpServer.Config.Handler = WebSocketHandler(backend, httpServer.Listener.Addr().String(), cap)
	httpServer.TLS = &tls.Config{MinVersion: tls.VersionTLS13}
	httpServer.StartTLS()
	t.Cleanup(func() { backend.Close(); httpServer.Close() })
	client := httpServer.Client()
	client.Transport.(*http.Transport).TLSClientConfig.MinVersion = tls.VersionTLS13
	return backend, httpServer, client
}
func websocketDial(t *testing.T, srv *httptest.Server, client *http.Client, headers http.Header) (*websocket.Conn, *http.Response, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	return websocket.Dial(ctx, strings.Replace(srv.URL, "https://", "wss://", 1)+"/relay", &websocket.DialOptions{HTTPClient: client, HTTPHeader: headers})
}
func websocketJoin(t *testing.T, ws *websocket.Conn, room, password string) (net.Conn, Frame) {
	t.Helper()
	conn := websocket.NetConn(context.Background(), ws, websocket.MessageBinary)
	conn.SetDeadline(time.Now().Add(2 * time.Second))
	payload, _ := json.Marshal(JoinRequest{Version: 1, Room: room, Password: password})
	if err := WriteFrame(conn, Frame{Type: Join, Payload: payload}); err != nil {
		t.Fatal(err)
	}
	welcome, err := ReadFrame(conn)
	if err != nil {
		t.Fatal(err)
	}
	return conn, welcome
}
func TestWebSocketTLSForwardAndRoomBoundaries(t *testing.T) {
	_, srv, client := websocketFixture(t, 8)
	a, response, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer a.CloseNow()
	if response.TLS == nil || response.TLS.Version != tls.VersionTLS13 || len(response.TLS.VerifiedChains) == 0 {
		t.Fatal("missing verified TLS1.3")
	}
	b, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer b.CloseNow()
	isolated, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer isolated.CloseNow()
	ac, aw := websocketJoin(t, a, "a", "test-room-password-aaaa")
	bc, bw := websocketJoin(t, b, "a", "test-room-password-aaaa")
	ic, iw := websocketJoin(t, isolated, "b", "test-room-password-bbbb")
	if aw.Type != Welcome || bw.Type != Welcome || iw.Type != Welcome || len(aw.Payload) != 8 {
		t.Fatal("welcome")
	}
	payload := []byte{0, 1, 255, 0, 128, 42}
	if err := WriteFrame(ac, Frame{Type: Packet, Payload: payload}); err != nil {
		t.Fatal(err)
	}
	got, err := ReadFrame(bc)
	if err != nil {
		t.Fatal(err)
	}
	if got.Type != Packet || string(got.Payload[8:]) != string(payload) || binary.BigEndian.Uint64(got.Payload[:8]) != binary.BigEndian.Uint64(aw.Payload) {
		t.Fatal("payload/source changed")
	}
	ic.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	if _, err := ReadFrame(ic); err == nil {
		t.Fatal("cross-room leak")
	}
	ac.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	if _, err := ReadFrame(ac); err == nil {
		t.Fatal("self echo")
	}
}
func TestWebSocketAdmissionAndHealth(t *testing.T) {
	_, srv, client := websocketFixture(t, 2)
	response, err := client.Get(srv.URL + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != 200 {
		t.Fatal(response.StatusCode)
	}
	req, _ := http.NewRequest(http.MethodGet, srv.URL+"/healthz", nil)
	req.Host = "other.invalid"
	response, err = client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != 421 {
		t.Fatal("Host not enforced")
	}
	_, response, err = websocketDial(t, srv, client, http.Header{"Origin": []string{"https://other.invalid"}})
	if err == nil || response.StatusCode != 403 {
		t.Fatal("cross-origin accepted")
	}
	a, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer a.CloseNow()
	b, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer b.CloseNow()
	_, response, err = websocketDial(t, srv, client, nil)
	if err == nil || response.StatusCode != 503 {
		t.Fatal("upgrade cap not enforced")
	}
}
func TestWebSocketAuthenticationAndMalformed(t *testing.T) {
	_, srv, client := websocketFixture(t, 8)
	bad, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer bad.CloseNow()
	_, answer := websocketJoin(t, bad, "a", "incorrect-password-long")
	if answer.Type != Failure {
		t.Fatal("bad password accepted")
	}
	text, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer text.CloseNow()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := text.Write(ctx, websocket.MessageText, []byte("not binary")); err != nil {
		t.Fatal(err)
	}
	if _, _, err := text.Read(ctx); err == nil {
		t.Fatal("text accepted")
	}
	oversized, _, err := websocketDial(t, srv, client, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer oversized.CloseNow()
	if err := oversized.Write(ctx, websocket.MessageBinary, make([]byte, MaxPayload+6)); err != nil {
		t.Fatal(err)
	}
	if _, _, err := oversized.Read(ctx); err == nil {
		t.Fatal("oversized message accepted")
	}
}
