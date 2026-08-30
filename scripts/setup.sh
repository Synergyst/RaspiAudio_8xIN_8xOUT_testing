#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

INSTALL_DEPS=1
BUILD=1
BUILD_WASM=1
INSTALL_IXWEBSOCKET=0
HOST_IP="${CM5AUDIO_HOST:-192.168.168.172}"

usage() {
    cat <<'EOF'
Usage: ./scripts/setup.sh [options]

Options:
  --no-install             Do not install Debian packages.
  --no-build               Only install/check prerequisites and create TLS files.
  --no-wasm                Do not build the browser WASM bundle.
  --install-ixwebsocket    Build and install a TLS-enabled ixwebsocket if missing.
  --host IP_OR_DNS         Hostname/IP to put in the development certificate.
  -h, --help               Show this help.

Environment:
  CM5AUDIO_HOST             Same as --host.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-install) INSTALL_DEPS=0 ;;
        --no-build) BUILD=0 ;;
        --no-wasm) BUILD_WASM=0 ;;
        --install-ixwebsocket) INSTALL_IXWEBSOCKET=1 ;;
        --host)
            [[ $# -ge 2 ]] || { echo "--host requires a value" >&2; exit 2; }
            HOST_IP="$2"
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [[ "$INSTALL_DEPS" -eq 1 ]]; then
    command -v apt-get >/dev/null 2>&1 || {
        echo "apt-get is required for automatic prerequisite installation." >&2
        exit 1
    }

    APT="apt-get"
    if [[ "$(id -u)" -ne 0 ]]; then
        command -v sudo >/dev/null 2>&1 || {
            echo "Run as root or install sudo to install prerequisites." >&2
            exit 1
        }
        APT="sudo apt-get"
    fi

    $APT update
    DEBIAN_FRONTEND=noninteractive $APT install -y \
        build-essential pkg-config cmake git openssl libssl-dev zlib1g-dev emscripten
fi

require_command() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing prerequisite: $1" >&2
        exit 1
    }
}

require_command g++
require_command make
require_command pkg-config
require_command openssl

if ! pkg-config --exists ixwebsocket; then
    if [[ "$INSTALL_IXWEBSOCKET" -eq 1 ]]; then
        ./scripts/install_ixwebsocket.sh
    else
        echo "Missing TLS-enabled ixwebsocket development package." >&2
        echo "Run ./scripts/setup.sh --install-ixwebsocket or install ixwebsocket under /usr/local." >&2
        exit 1
    fi
fi

[[ -f /usr/local/include/ixwebsocket/IXWebSocketServer.h || \
   -f /usr/include/ixwebsocket/IXWebSocketServer.h ]] || {
    echo "ixwebsocket headers were not found." >&2
    exit 1
}

mkdir -p certs
CERT_CONFIG="$(mktemp)"
trap 'rm -f "$CERT_CONFIG"' EXIT
if [[ "$HOST_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    SAN_ENTRY="IP:$HOST_IP"
else
    SAN_ENTRY="DNS:$HOST_IP"
fi

cat > "$CERT_CONFIG" <<EOF
[req]
distinguished_name = req_distinguished_name
x509_extensions = v3_req
prompt = no

[req_distinguished_name]
CN = $HOST_IP

[v3_req]
subjectAltName = $SAN_ENTRY
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF

CERT_NEEDS_GENERATION=0
if [[ ! -s certs/server.crt || ! -s certs/server.key ]]; then
    CERT_NEEDS_GENERATION=1
elif ! openssl x509 -in certs/server.crt -noout >/dev/null 2>&1 || \
     ! openssl pkey -in certs/server.key -noout >/dev/null 2>&1 || \
     [[ "$(openssl x509 -in certs/server.crt -pubkey -noout | openssl pkey -pubin -outform DER 2>/dev/null | sha256sum | cut -d' ' -f1)" != \
        "$(openssl pkey -in certs/server.key -pubout -outform DER 2>/dev/null | sha256sum | cut -d' ' -f1)" ]]; then
    echo "Existing TLS certificate or key is invalid or mismatched; regenerating."
    CERT_NEEDS_GENERATION=1
elif [[ "$HOST_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    openssl verify -CAfile certs/server.crt -verify_ip "$HOST_IP" certs/server.crt >/dev/null 2>&1 || CERT_NEEDS_GENERATION=1
else
    openssl verify -CAfile certs/server.crt -verify_hostname "$HOST_IP" certs/server.crt >/dev/null 2>&1 || CERT_NEEDS_GENERATION=1
fi

if [[ "$CERT_NEEDS_GENERATION" -eq 1 ]]; then
    echo "Generating development TLS certificate for $HOST_IP"
    openssl req -x509 -nodes -newkey rsa:2048 -sha256 -days 825 \
        -keyout certs/server.key \
        -out certs/server.crt \
        -config "$CERT_CONFIG" \
        >/dev/null 2>&1
else
    echo "Using existing certs/server.crt and certs/server.key"
fi

chmod 600 certs/server.key
chmod 644 certs/server.crt
openssl x509 -in certs/server.crt -noout -subject -ext subjectAltName

if [[ "$BUILD" -eq 1 ]]; then
    make clean
    make
fi

if [[ "$BUILD_WASM" -eq 1 ]]; then
    ./web_client/build_wasm.sh
fi

echo "Setup complete. Start the engine with: ./cm5audio"
echo "For each Ubuntu Brave/Chromium client, copy certs/server.crt and scripts/trust_server_cert.sh, then run as the browser user:"
echo "  ./scripts/trust_server_cert.sh /path/to/server.crt"
