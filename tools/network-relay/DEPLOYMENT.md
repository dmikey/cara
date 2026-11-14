# Private relay deployment

Target: `carap2p.therad.ninja`, existing RadOps host. Service endpoint is `wss://carap2p.therad.ninja/relay`; readiness endpoint is `https://carap2p.therad.ninja/healthz`.

Deployment status: **pending remote activation/verification**. Source, tests and packaging are prepared on branch `feature/network-relay-deployment-20261113`. A live receipt will replace this status only after trusted remote forwarding passes.

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
