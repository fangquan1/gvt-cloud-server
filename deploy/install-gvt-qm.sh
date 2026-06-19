#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

install -D -m 0755 "$repo_dir/scripts/gvt-qm" /usr/local/bin/gvt-qm
install -D -m 0755 "$repo_dir/scripts/gvt-qm-run" /usr/local/sbin/gvt-qm-run
install -D -m 0644 "$repo_dir/deploy/systemd/gvt-qemu@.service" /etc/systemd/system/gvt-qemu@.service
install -d -m 0755 /etc/gvt-qm

if [ ! -e /etc/gvt-qm/win10.conf ]; then
  install -m 0644 "$repo_dir/deploy/gvt-qm/win10.conf.example" /etc/gvt-qm/win10.conf
  echo "Installed example VM config: /etc/gvt-qm/win10.conf"
else
  echo "Keeping existing VM config: /etc/gvt-qm/win10.conf"
fi

systemctl daemon-reload

cat <<'EOF'
Installed gvt-qm.

Try:
  gvt-qm status win10
  gvt-qm start win10
  gvt-qm logs win10
  gvt-qm stop win10
EOF
