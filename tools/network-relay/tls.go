// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
package relay

import (
	"crypto/tls"
	"net"
)

// TLSListener is the relay's remote transport. The caller owns certificate
// provisioning; clients must verify its chain and hostname using their trust store.
func TLSListener(listener net.Listener, certificate tls.Certificate) net.Listener {
	return tls.NewListener(listener, &tls.Config{
		Certificates: []tls.Certificate{certificate},
		MinVersion:   tls.VersionTLS13,
	})
}
