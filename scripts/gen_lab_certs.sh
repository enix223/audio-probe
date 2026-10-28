#!/usr/bin/env bash
# Generate a self-signed lab CA and a server certificate for the local MQTT
# broker. Output goes to out/certs (gitignored). Never use these in production.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$ROOT/out/certs"
mkdir -p "$DIR"
cd "$DIR"

if [[ -f ca.crt && -f server.crt && -f server.key ]]; then
  echo "certificates already exist in $DIR"
  exit 0
fi

openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt \
  -days 3650 -subj "/CN=AudioProbe Lab CA" >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -subj "/CN=127.0.0.1" >/dev/null 2>&1
cat > san.cnf <<EOF
subjectAltName=IP:127.0.0.1,DNS:localhost
EOF
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 3650 -extfile san.cnf >/dev/null 2>&1
rm -f server.csr san.cnf
echo "generated lab certificates in $DIR"
