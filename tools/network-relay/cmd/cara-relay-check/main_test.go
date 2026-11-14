// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package main

import (
	"context"
	"github.com/coder/websocket"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

func TestVerificationClientRejectsRedirects(t *testing.T) {
	var destinationRequests atomic.Int32
	destination := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		destinationRequests.Add(1)
		w.WriteHeader(http.StatusOK)
	}))
	defer destination.Close()
	for _, status := range []int{301, 302, 303, 307, 308} {
		origin := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, destination.URL, status)
		}))
		response, err := verificationClient().Get(origin.URL)
		if response != nil {
			response.Body.Close()
		}
		origin.Close()
		if err == nil {
			t.Fatalf("followed status %d", status)
		}
	}
	if destinationRequests.Load() != 0 {
		t.Fatal("redirect destination was contacted")
	}
}

func TestVerificationWebSocketRejectsRedirects(t *testing.T) {
	var destinationRequests atomic.Int32
	destination := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		destinationRequests.Add(1)
		w.WriteHeader(http.StatusBadRequest)
	}))
	defer destination.Close()
	for _, status := range []int{301, 302, 303, 307, 308} {
		origin := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, destination.URL, status)
		}))
		conn, response, err := websocket.Dial(context.Background(), "ws"+strings.TrimPrefix(origin.URL, "http"), &websocket.DialOptions{HTTPClient: verificationClient()})
		if conn != nil {
			conn.CloseNow()
		}
		if response != nil {
			response.Body.Close()
		}
		origin.Close()
		if err == nil {
			t.Fatalf("followed WebSocket redirect %d", status)
		}
	}
	if destinationRequests.Load() != 0 {
		t.Fatal("WebSocket redirect destination was contacted")
	}
}
