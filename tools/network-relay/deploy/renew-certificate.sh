#!/bin/sh
set -eu
# certbot deploy hook; never changes another hostname's configuration.
if [ "${RENEWED_LINEAGE:-}" = /etc/letsencrypt/live/carap2p.therad.ninja ]; then
    /usr/sbin/nginx -t
    /bin/systemctl reload nginx
fi
