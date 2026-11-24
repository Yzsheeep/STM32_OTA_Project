#!/bin/sh
# Certbot deploy-hook scoped to the public firmware certificate.
set -eu
[ "${RENEWED_LINEAGE:-}" = /etc/letsencrypt/live/f411-ota-ip ] || exit 0
/usr/bin/docker exec f411-ota-https nginx -t
/usr/bin/docker exec f411-ota-https nginx -s reload
