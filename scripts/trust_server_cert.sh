#!/usr/bin/env bash
set -euo pipefail

# Install a CM5 Audio self-signed server certificate for the current desktop
# user. Run this as the user who launches Brave/Chromium; do not run `sudo su -`
# before invoking it. sudo is used only for the system trust store.

usage() {
    cat <<'EOF'
Usage: ./scripts/trust_server_cert.sh CERTIFICATE

Install CERTIFICATE into:
  1. The Ubuntu system CA store (requires sudo).
  2. The current user's NSS store, used by Brave/Chromium (requires certutil).

Run as the normal desktop user, not root. Fully restart Brave/Chromium after
running this script.
EOF
}

if [[ $# -ne 1 || "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    [[ $# -eq 1 ]] && exit 0 || exit 2
fi

if [[ "$(id -u)" -eq 0 ]]; then
    echo 'Run this as the normal browser user, not root or sudo su -.' >&2
    exit 1
fi

CERT_SOURCE="$1"
[[ -f "$CERT_SOURCE" ]] || {
    echo "Certificate not found: $CERT_SOURCE" >&2
    exit 1
}
CERT_SOURCE="$(realpath "$CERT_SOURCE")"

command -v openssl >/dev/null 2>&1 || {
    echo "Missing prerequisite: openssl" >&2
    exit 1
}
openssl x509 -in "$CERT_SOURCE" -noout >/dev/null 2>&1 || {
    echo "Not a readable PEM certificate: $CERT_SOURCE" >&2
    exit 1
}

CERT_NAME="cm5audio"
SYSTEM_CERT="/usr/local/share/ca-certificates/${CERT_NAME}.crt"

sudo install -D -m 0644 "$CERT_SOURCE" "$SYSTEM_CERT"
sudo update-ca-certificates

if ! command -v certutil >/dev/null 2>&1; then
    echo
    echo "System trust installed, but certutil is missing; Brave/Chromium may need NSS import." >&2
    echo "Install it with: sudo apt install libnss3-tools" >&2
    echo "Then rerun this script." >&2
    exit 0
fi

NSS_DIR="$HOME/.pki/nssdb"
mkdir -p "$NSS_DIR"
if [[ ! -f "$NSS_DIR/cert9.db" ]]; then
    certutil -N -d "sql:$NSS_DIR" --empty-password
fi

# P,, trusts this exact self-signed peer certificate. It does not treat the
# server certificate as a certificate authority, which avoids Brave rejecting
# a self-signed leaf certificate that is not marked CA:TRUE.
certutil -D -d "sql:$NSS_DIR" -n CM5Audio 2>/dev/null || true
certutil -A \
    -d "sql:$NSS_DIR" \
    -n CM5Audio \
    -t "P,," \
    -i "$CERT_SOURCE"

FINGERPRINT="$(openssl x509 -in "$CERT_SOURCE" -noout -fingerprint -sha256 | cut -d= -f2-)"
echo
echo "CM5 Audio certificate installed for system trust and Brave/Chromium."
echo "SHA-256: $FINGERPRINT"
echo "Fully restart Brave/Chromium, then open the HTTPS client URL."
