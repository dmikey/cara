// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"strings"
	"time"

	"github.com/coder/websocket"
)

type immediateWebSocketConn struct {
	net.Conn
	ws *websocket.Conn
}

func (c *immediateWebSocketConn) Close() error { return c.ws.CloseNow() }

// WebSocketHandler carries the same v1 byte stream as ServeConn over binary
// WebSocket messages. Deploy behind HTTPS; its HTTP listener must be loopback.
// It does not accept a room or password in a URL, header, or health response.
func WebSocketHandler(server *Server, publicHost string, maxConnections int) http.Handler {
	slots := make(chan struct{}, maxConnections)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		if !strings.EqualFold(r.Host, publicHost) {
			http.Error(w, "unexpected host", http.StatusMisdirectedRequest)
			return
		}
		if r.URL.Path == "/healthz" {
			if r.Method != http.MethodGet {
				w.WriteHeader(http.StatusMethodNotAllowed)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(map[string]interface{}{"service": "cara-relay", "status": "ok", "protocol": 1, "transport": "websocket"})
			return
		}
		if r.URL.Path != "/relay" {
			http.NotFound(w, r)
			return
		}
		if r.Method != http.MethodGet || r.URL.RawQuery != "" {
			http.Error(w, "upgrade requires GET without a query", http.StatusBadRequest)
			return
		}
		select {
		case slots <- struct{}{}:
			defer func() { <-slots }()
		default:
			http.Error(w, "connection limit", http.StatusServiceUnavailable)
			return
		}
		// Default same-host Origin validation remains enabled. Native clients may
		// omit Origin. Compression is disabled to bound per-connection state.
		ws, err := websocket.Accept(w, r, &websocket.AcceptOptions{CompressionMode: websocket.CompressionDisabled})
		if err != nil {
			return
		}
		defer ws.CloseNow()
		ws.SetReadLimit(MaxPayload + 5)
		ctx, cancel := context.WithCancel(r.Context())
		defer cancel()
		conn := &immediateWebSocketConn{Conn: websocket.NetConn(ctx, ws, websocket.MessageBinary), ws: ws}
		defer conn.Close()
		go func() {
			ticker := time.NewTicker(25 * time.Second)
			defer ticker.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
					pingCtx, done := context.WithTimeout(ctx, 5*time.Second)
					err := ws.Ping(pingCtx)
					done()
					if err != nil {
						conn.Close()
						return
					}
				}
			}
		}()
		server.ServeConn(conn)
	})
}
