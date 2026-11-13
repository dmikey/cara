// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package main

import (
	"crypto/tls"
	"encoding/json"
	"flag"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"

	relay "cara.network/relay"
)

func main() {
	address := flag.String("listen", "127.0.0.1:24873", "listen address (remote listeners require TLS)")
	certPath := flag.String("tls-cert", "", "PEM server certificate")
	keyPath := flag.String("tls-key", "", "PEM server private key")
	maxPeers := flag.Int("max-peers", 32, "total connection limit")
	queueDepth := flag.Int("queue-depth", 64, "per-peer packet queue limit")
	flag.Parse()
	var rooms map[string]string
	if json.Unmarshal([]byte(os.Getenv("CARA_RELAY_ROOMS")), &rooms) != nil {
		log.Fatal("set CARA_RELAY_ROOMS to a JSON object of room names and passwords")
	}
	server, err := relay.New(relay.Config{Rooms: rooms, MaxPeers: *maxPeers, QueueDepth: *queueDepth})
	if err != nil {
		log.Fatal(err)
	}
	host, _, err := net.SplitHostPort(*address)
	if err != nil {
		log.Fatal(err)
	}
	if *certPath == "" || *keyPath == "" {
		ip := net.ParseIP(host)
		if ip == nil || !ip.IsLoopback() {
			log.Fatal("non-loopback listeners require -tls-cert and -tls-key")
		}
		if *certPath != "" || *keyPath != "" {
			log.Fatal("both TLS files are required")
		}
	}
	listener, err := net.Listen("tcp", *address)
	if err != nil {
		log.Fatal(err)
	}
	if *certPath != "" {
		cert, err := tls.LoadX509KeyPair(*certPath, *keyPath)
		if err != nil {
			listener.Close()
			log.Fatal(err)
		}
		listener = relay.TLSListener(listener, cert)
	}
	defer listener.Close()
	defer server.Close()
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(stop)
	go func() { <-stop; listener.Close(); server.Close() }()
	log.Printf("Cara packet relay listening on %s", listener.Addr())
	for {
		conn, err := listener.Accept()
		if err != nil {
			if err != net.ErrClosed {
				log.Print(err)
			}
			return
		}
		if tcp, ok := conn.(*net.TCPConn); ok {
			tcp.SetNoDelay(true)
			tcp.SetKeepAlive(true)
		}
		go server.ServeConn(conn)
	}
}
