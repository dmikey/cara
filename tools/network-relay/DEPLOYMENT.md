# Private relay deployment

Target: `carap2p.therad.ninja`, existing RadOps host. Service endpoint is `wss://carap2p.therad.ninja/relay`; readiness endpoint is `https://carap2p.therad.ninja/healthz`.

Deployment status: **running and remotely verified**, 2026-11-14 UTC (November 13 local time). Source branch: `feature/network-relay-deployment-20261113`. Deployed source revision: `475cc245964e769b35d3c6ed366117eb48e5a889`. Immutable image: `sha256:d480f944bd7d1559b4cfc9102253a61761c4487d616217d02bf7218a2062a1a3`. Built with the official SHA-verified Go1.27.4 toolchain.

The external Mac verification client used system trust and the actual public hostname, required/negotiated TLS1.3, joined three private peers, forwarded an exact4,088-byte binary payload with its source tag, checked no echo and room isolation, rejected an incorrect password, rejected wrong-hostname/untrusted-CA connections, and checked public HTTPS health. No certificate-validation bypass or DNS override was used. Temporary credentials from the first probe were deleted. The final post-recovery probe supplied credentials only through process stdin, with no local credential file; persistent credentials remain server-only and passwords were never printed.

Host configuration was changed under the existing production lock after an encrypted nginx snapshot was decrypted, parsed and hash-verified. The final receipt confirms every preexisting container ID and nginx file hash remained unchanged. Only the new Cara container/vhost and its certificate/renewal hook were added; existing applications were not restarted. DNS and Cloudflare zone policies were not changed. The tenant data volume was not used. Two initial attempts rolled back safely (an edge user-agent rejection, then an asynchronous reload readiness check); their protected backup/release files remain on the host. The installer now identifies its service probes and waits for exact public readiness. A packaging health-check correction then changed the scratch image to an exec-form Docker health check (no shell); only Cara's container was updated, preserving private rooms and nginx. The prior Cara container is retained stopped as `cara-relay-rollback-9e2ef26aabe5` for scoped rollback.

Private host state: `/var/snap/docker/common/cara-relay/`. Room configuration is under its `private/rooms.json`, mode0400/UID65532, behind a root-controlled directory and bound read-only into the service. It contains the private room plus isolated verification rooms. Retrieve/distribute credentials only through an authenticated administrative channel; they are intentionally omitted here. `receipt.json` and `supervision-proof.json` retain non-secret deployment/recovery evidence. An isolated main-process failure was injected: Docker automatically restarted the same current container (restart count0→1), it became healthy, and the full external TLS1.3 packet proof passed again. Its listener was verified as127.0.0.1:24874, user65532, read-only filesystem, all capabilities dropped,64MiB memory,0.25CPU,48processes,256descriptors and two2MiB log files. Local artifact receipts are under `/Volumes/Fast/CaraBuild/network-relay-artifacts-20261113/`.

The current update adds opt-in server membership notifications and authenticated reconnect epochs while preserving the legacy protocol stream. The membership tests passed three runs under Go's race detector, including sequential and concurrent replacement. The actual Foundation client then passed public-WSS maximum-packet/reply, no-echo, private-room and wrong-password checks, followed by forty same-MAC reconnects with forty distinct server source IDs. The observer verified a trusted departure before each rebinding. These are transport proofs, not UDS or Pokémon gameplay. The update receipt confirms the private room file, nginx and unrelated container IDs remained unchanged; the service is healthy. Local non-secret receipts are `membership-deployment.log`, `foundation-process-proof.log` and `foundation-reconnect-proof.log` in the artifact directory above.

The transport is protocol v1 inside binary WebSocket messages. Cara's ENet adapter, iOS UI/lifecycle, StreetPass exchange and named game actions remain unimplemented/unverified. Deployment alone enables none of them.

## Isolation and operation

Use the existing nginx edge and unchanged DNS. Install only the supplied hostname-specific HTTP/HTTPS blocks; do not replace global nginx configuration or other virtual hosts. Validate `nginx -t` before graceful reload. Do not restart existing application containers. Keep relay release files and bounded logs on the host root filesystem; the RadOps tenant data volume is outside this deployment's storage scope.

Run the supplied scratch image with `deploy/compose.yml`, pinned by immutable image ID. Room configuration is a server-generated map of names to high-entropy passwords in a root-controlled directory, bound read-only as `/rooms.json` with UID65532/mode0400. Credentials must not be committed, placed in URLs, printed, or included in deployment receipts. Changing rooms requires replacing this file safely and restarting **only** `cara-relay`; current relay sessions disconnect.

The Docker health check uses only local `/healthz`. The public verification client requires a system-trusted hostname and TLS1.3; shared Cloudflare minimum-TLS policy is not modified. The supplied origin vhost selects TLS1.3. A successful public TLS1.3 connection does not assert that Cloudflare rejects every older TLS handshake.

Acquire `/run/lock/radops-production-backup.lock` before host configuration changes. Back up/restore-verify the affected configuration and record existing container IDs and nginx configuration hashes. No tenant DB, upload, billing, user or application state is modified by this isolated service. Preserve unrelated files and containers.

## Rollback

1. Stop/remove only `cara-relay` and keep its private configuration protected for recovery.
2. Remove only the `carap2p.therad.ninja` nginx site/symlink and the certificate's dedicated renewal hook.
3. Run `nginx -t`, then gracefully reload nginx. Existing applications and their certificates remain untouched.
4. If decommissioning permanently, remove only this hostname's unused certificate with certbot; do not remove other lineages or the shared ACME account.
5. Compare unrelated container IDs and nginx file hashes to the pre-deployment receipt and verify existing service health.

A future update should create an immutable release, test it before activation, retain the prior image/release for rollback and restart only this service. No paid resource or DNS mutation is required by this design.

`deploy/install.py` performs guarded initial deployment; `deploy/update.py` performs a guarded replacement of only the existing Cara service with a pinned release. Both default to read-only preflight; `--apply` is explicit. Updates retain the prior Cara container stopped until deliberate rollback/cleanup, verify the private room file is unchanged, and validate the new exec-form image health check before activation. Do not run initial install over existing state.
