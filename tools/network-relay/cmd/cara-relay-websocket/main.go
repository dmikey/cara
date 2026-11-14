// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	relay "cara.network/relay"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:24874", "loopback HTTP listener behind an HTTPS reverse proxy")
	roomsFile := flag.String("rooms-file", "", "private JSON room/password file")
	publicHost := flag.String("public-host", "", "exact public Host header (no scheme or path)")
	maxPeers := flag.Int("max-peers", 32, "total connection limit")
	queue := flag.Int("queue-depth", 64, "per-peer queue depth")
	health := flag.String("healthcheck", "", "check this local HTTP health URL, then exit")
	flag.Parse()
	if *health != "" {
		client := http.Client{Timeout: 4 * time.Second}
		req, err := http.NewRequest(http.MethodGet, *health, nil)
		if err != nil {
			log.Fatal("invalid health URL")
		}
		req.Host = *publicHost
		response, err := client.Do(req)
		if err != nil {
			log.Fatal("relay health unavailable")
		}
		defer response.Body.Close()
		if response.StatusCode != http.StatusOK {
			log.Fatal("relay health failed")
		}
		var body struct {
			Service, Status, Transport string
			Protocol                   int
		}
		if json.NewDecoder(io.LimitReader(response.Body, 1024)).Decode(&body) != nil || body.Service != "cara-relay" || body.Status != "ok" || body.Protocol != 1 || body.Transport != "websocket" {
			log.Fatal("unexpected health response")
		}
		return
	}
	host, _, err := net.SplitHostPort(*listen)
	if err != nil || net.ParseIP(host) == nil || !net.ParseIP(host).IsLoopback() {
		log.Fatal("WebSocket HTTP listener must be a literal loopback address; expose it through HTTPS")
	}
	if *publicHost == "" || strings.ContainsAny(*publicHost, "/:\\?#@ \t\r\n") {
		log.Fatal("set a DNS hostname with -public-host")
	}
	if *roomsFile == "" {
		log.Fatal("set -rooms-file")
	}
	file, err := os.Open(*roomsFile)
	if err != nil {
		log.Fatal("cannot open private room configuration")
	}
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > 65536 {
		file.Close()
		log.Fatal("room file must be regular, private (mode 0600 or 0400), and at most 64 KiB")
	}
	var rooms map[string]string
	decoder := json.NewDecoder(io.LimitReader(file, 65537))
	err = decoder.Decode(&rooms)
	var extra interface{}
	if err == nil && decoder.Decode(&extra) != io.EOF {
		err = fmt.Errorf("trailing configuration")
	}
	file.Close()
	if err != nil {
		log.Fatal("invalid private room configuration")
	}
	server, err := relay.New(relay.Config{Rooms: rooms, MaxPeers: *maxPeers, QueueDepth: *queue})
	if err != nil {
		log.Fatal(err)
	}
	defer server.Close()
	listener, err := net.Listen("tcp", *listen)
	if err != nil {
		log.Fatal(err)
	}
	httpServer := &http.Server{Handler: relay.WebSocketHandler(server, *publicHost, *maxPeers), ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 15 * time.Second, MaxHeaderBytes: 16 * 1024}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		server.Close()
		end, done := context.WithTimeout(context.Background(), 5*time.Second)
		defer done()
		httpServer.Shutdown(end)
	}()
	log.Printf("Cara WebSocket relay listening on %s for host %s", listener.Addr(), *publicHost)
	if err = httpServer.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal("relay listener failed")
	}
}
