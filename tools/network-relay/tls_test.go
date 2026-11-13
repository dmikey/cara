// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"errors"
	"math/big"
	"net"
	"testing"
	"time"
)

func ephemeralCertificate(t *testing.T) (tls.Certificate, *x509.CertPool) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	ca := &x509.Certificate{
		SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "Cara ephemeral test CA"},
		NotBefore: now.Add(-time.Minute), NotAfter: now.Add(time.Hour),
		IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, ca, ca, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	parsedCA, err := x509.ParseCertificate(caDER)
	if err != nil {
		t.Fatal(err)
	}
	serverKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	server := &x509.Certificate{
		SerialNumber: big.NewInt(2), Subject: pkix.Name{CommonName: "relay.cara.test"},
		DNSNames: []string{"relay.cara.test"}, NotBefore: now.Add(-time.Minute), NotAfter: now.Add(time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, server, parsedCA, &serverKey.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	pool.AddCert(parsedCA)
	return tls.Certificate{Certificate: [][]byte{der, caDER}, PrivateKey: serverKey}, pool
}

func TestTLSVerifiedClientsForwardAndRejectInvalidTrust(t *testing.T) {
	certificate, roots := ephemeralCertificate(t)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	_, address := launchListener(t, TLSListener(listener, certificate))
	dial := func(config *tls.Config) (*tls.Conn, error) {
		return tls.DialWithDialer(&net.Dialer{Timeout: time.Second}, "tcp", address, config)
	}
	config := &tls.Config{RootCAs: roots, ServerName: "relay.cara.test", MinVersion: tls.VersionTLS13}
	a, err := dial(config)
	if err != nil {
		t.Fatal(err)
	}
	_, id := joinConn(t, a, "alpha")
	b, err := dial(config)
	if err != nil {
		t.Fatal(err)
	}
	joinConn(t, b, "alpha")
	if a.ConnectionState().Version != tls.VersionTLS13 || b.ConnectionState().Version != tls.VersionTLS13 {
		t.Fatal("TLS 1.3 was not negotiated")
	}
	payload := []byte{0, 255, 1, 0, 9, 128, 7}
	if err := WriteFrame(a, Frame{Packet, payload}); err != nil {
		t.Fatal(err)
	}
	b.SetReadDeadline(time.Now().Add(time.Second))
	frame, err := ReadFrame(b)
	if err != nil || frame.Type != Packet || len(frame.Payload) != len(payload)+8 || binary.BigEndian.Uint64(frame.Payload[:8]) != id || !bytes.Equal(frame.Payload[8:], payload) {
		t.Fatalf("TLS payload changed: %v %v", frame, err)
	}
	for _, test := range []struct {
		name      string
		config    *tls.Config
		unknownCA bool
	}{
		{"untrusted CA", &tls.Config{RootCAs: x509.NewCertPool(), ServerName: "relay.cara.test", MinVersion: tls.VersionTLS13}, true},
		{"wrong hostname", &tls.Config{RootCAs: roots, ServerName: "different.cara.test", MinVersion: tls.VersionTLS13}, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			conn, err := dial(test.config)
			if conn != nil {
				conn.Close()
			}
			if err == nil {
				t.Fatal("invalid server trust accepted")
			}
			if test.unknownCA {
				var unknown x509.UnknownAuthorityError
				if !errors.As(err, &unknown) {
					t.Fatalf("wrong rejection: %v", err)
				}
			} else {
				var hostname x509.HostnameError
				if !errors.As(err, &hostname) {
					t.Fatalf("wrong rejection: %v", err)
				}
			}
		})
	}
	legacy := config.Clone()
	legacy.MinVersion = tls.VersionTLS12
	legacy.MaxVersion = tls.VersionTLS12
	conn, err := dial(legacy)
	if conn != nil {
		conn.Close()
	}
	if err == nil {
		t.Fatal("TLS 1.2 accepted by TLS 1.3-only relay")
	}
}
