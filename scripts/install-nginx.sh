#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Adam DePrince
# Run as root on hail with the reviewed config as the first argument.
set -eu
candidate=$1
available=/etc/nginx/sites-available/scry.goblinreactor.com
enabled=/etc/nginx/sites-enabled/scry.goblinreactor.com
test "$(id -u)" = 0
test -f "$candidate"
nginx -t
# Preserve TLS configuration if a separately managed HTTPS vhost already exists.
if test -f "$available" && grep -Eq 'listen[[:space:]].*443|ssl_certificate' "$available"; then
    echo 'Existing Scry vhost has TLS configuration. Merge the reviewed location block into that vhost before applying.' >&2
    exit 1
fi
if test -e "$enabled" || test -L "$enabled"; then
    test -L "$enabled" && test "$(readlink -f "$enabled")" = "$available" || {
        echo 'Unexpected existing sites-enabled entry; inspect it before replacing.' >&2
        exit 1
    }
fi
backup=$(mktemp -d /var/tmp/goblin-scry-nginx-backup.XXXXXX)
test ! -e "$available" || cp -a "$available" "$backup/available"
test ! -L "$enabled" || cp -a "$enabled" "$backup/enabled"
restore() {
    if test -e "$backup/available"; then cp -a "$backup/available" "$available"; else rm -f "$available"; fi
    rm -f "$enabled"
    test ! -L "$backup/enabled" || cp -a "$backup/enabled" "$enabled"
}
install -m 0644 "$candidate" "$available"
ln -sfn "$available" "$enabled"
if ! nginx -t; then
    restore
    echo "Rejected candidate; restored configuration. Backup: $backup" >&2
    exit 1
fi
if ! systemctl reload nginx; then
    restore
    nginx -t && systemctl reload nginx
    echo "Reload failed; restored configuration. Backup: $backup" >&2
    exit 1
fi
printf 'Installed Scry origin route. Previous configuration: %s\n' "$backup"
