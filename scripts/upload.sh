#!/usr/bin/env bash
# Upload the built boot.firm to a 3DS running ftpd (anonymous FTP) over wifi.
#
# Usage: scripts/upload.sh [--build] [--boot] [--help]
#
#   default   upload to /luma/payloads/x_gauge.firm (chainload it by holding X at boot)
#   --boot    upload to /boot.firm instead; the existing remote /boot.firm is downloaded
#             to scripts/backups/boot.firm.<timestamp> first
#   --build   run `make` in the repository root before uploading
#
# The 3DS IP comes from $THREEDS_IP or the file scripts/.3ds-ip (one line, gitignored).
# Port: $THREEDS_PORT (default 5000, ftpd's default).
#
# AI disclosure: written with the help of AI (Claude Code), reviewed by the maintainer.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRM="$REPO_DIR/boot.firm"
IP_FILE="$SCRIPT_DIR/.3ds-ip"
BACKUP_DIR="$SCRIPT_DIR/backups"

die() { echo "error: $*" >&2; exit 1; }

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; }

do_build=0
do_boot=0
for arg in "$@"; do
    case "$arg" in
        --build) do_build=1 ;;
        --boot)  do_boot=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $arg" ;;
    esac
done

command -v curl >/dev/null 2>&1 || die "curl not found"

ip="${THREEDS_IP:-}"
if [[ -z "$ip" && -f "$IP_FILE" ]]; then
    ip="$(head -n 1 "$IP_FILE" | tr -d '[:space:]')"
fi
[[ -n "$ip" ]] || die "3DS IP unknown: set THREEDS_IP or write it to $IP_FILE"
port="${THREEDS_PORT:-5000}"
base="ftp://$ip:$port"

if (( do_build )); then
    echo "==> make"
    make -C "$REPO_DIR"
fi

[[ -f "$FIRM" ]] || die "$FIRM not found (build first, or pass --build)"

curl_ftp=(curl --fail --show-error --silent --user anonymous: --connect-timeout 10)

if (( do_boot )); then
    remote="/boot.firm"
    mkdir -p "$BACKUP_DIR"
    backup="$BACKUP_DIR/boot.firm.$(date +%Y%m%d-%H%M%S)"
    echo "==> backing up $base$remote to $backup"
    "${curl_ftp[@]}" -o "$backup" "$base$remote" || die "could not download the existing $remote (not overwriting it)"
    [[ -s "$backup" ]] || die "backup $backup is empty (not overwriting $remote)"
else
    remote="/luma/payloads/x_gauge.firm"
fi

echo "==> uploading $(basename "$FIRM") ($(wc -c < "$FIRM" | tr -d ' ') bytes) to $base$remote"
"${curl_ftp[@]}" --ftp-create-dirs -T "$FIRM" "$base$remote"
echo "==> done"
if (( ! do_boot )); then
    echo "    hold X while powering on to boot it (Luma chainloader: x_*.firm)"
fi
