#!/bin/sh
# Certbot pre-hook. Install as root; the parent directory is private to root.
set -eu
webroot=/var/lib/f411-certbot
pid=$(/usr/bin/docker inspect --format '{{.State.Pid}}' my-nginx)
case "$pid" in ''|0|*[!0-9]*) exit 1 ;; esac
site=/proc/$pid/root/usr/share/nginx/html
test -d "$site"
/usr/bin/docker exec my-nginx mkdir -p /usr/share/nginx/html/.well-known/acme-challenge
test -d "$site/.well-known/acme-challenge"
# Only this private host-side link changes. Keep the webroot directory itself stable.
ln -sfnT "$site/.well-known" "$webroot/.well-known"
