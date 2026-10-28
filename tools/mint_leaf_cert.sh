#!/usr/bin/env bash
# Mint a TLS server certificate for one host name, signed by our private root, on the Mac.
#
#   tools/mint_leaf_cert.sh HOST            e.g. tools/mint_leaf_cert.sh test.example.com
#
# The patched HTTPS APK trusts exactly one root, signing/bf-root-ca.pem (patch_apk.py --root-ca),
# so every host it talks to over https needs a leaf from that root, not a public certificate.
# This writes signing/HOST.key and signing/HOST.pem (both 0600; signing/ is gitignored and the
# root key never leaves this machine), made like signing/api.example.com.pem
# (re/notes/https_transfer_re.json section 4): EC P-256, ecdsa-with-SHA256, CN and SAN DNS:HOST,
# basicConstraints critical CA:FALSE, keyUsage critical digitalSignature, extendedKeyUsage
# serverAuth, subject/authority key ids, 825 days in all. It starts a day in the past so a phone
# whose clock is a little behind still accepts it.
#
# It refuses to overwrite either file, checks the chain with `openssl verify -purpose sslserver
# -verify_hostname HOST` and that the key matches the certificate, and never prints the key.
#
# Settings (environment):
#   SIGNING_DIR  folder holding bf-root-ca.{pem,key} (default: signing/ of this checkout, or of the
#                main checkout when this is a git worktree, since signing/ exists only there)
#   OPENSSL      an OpenSSL 3.4 or newer (default: openssl on PATH; macOS's /usr/bin/openssl is
#                LibreSSL, which lacks -not_before: use Homebrew's)
set -euo pipefail
umask 077

die() { echo "mint_leaf_cert.sh: $*" >&2; exit 1; }

(( $# == 1 )) || die 'usage: tools/mint_leaf_cert.sh HOST'
HOST=$1
# A plain lowercase DNS name: no wildcard, no port, no IP (the client checks the dNSName SAN).
[[ ${#HOST} -le 253 && $HOST =~ ^([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$ ]] \
    || die "not a lowercase host name: $HOST"

OPENSSL=${OPENSSL:-openssl}
version=$("$OPENSSL" version 2>/dev/null) || die "no $OPENSSL"
[[ $version =~ ^OpenSSL\ ([0-9]+)\.([0-9]+) ]] \
    || die "$OPENSSL is \"$version\"; set OPENSSL to an OpenSSL 3.4+ (e.g. /opt/homebrew/bin/openssl)"
(( BASH_REMATCH[1] > 3 || (BASH_REMATCH[1] == 3 && BASH_REMATCH[2] >= 4) )) \
    || die "$OPENSSL is $version; -not_before needs OpenSSL 3.4 or newer"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [[ -n ${SIGNING_DIR:-} ]]; then
    SIGNING=$SIGNING_DIR
else
    SIGNING=$ROOT/signing
    if [[ ! -e $SIGNING/bf-root-ca.pem ]] \
            && common=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null); then
        SIGNING=$(dirname "$common")/signing          # a worktree: the main checkout holds the keys
    fi
fi
CA=$SIGNING/bf-root-ca.pem
CAKEY=$SIGNING/bf-root-ca.key
KEY=$SIGNING/$HOST.key
PEM=$SIGNING/$HOST.pem
[[ -f $CA && -f $CAKEY ]] || die "no bf-root-ca.pem and bf-root-ca.key in $SIGNING"
for f in "$KEY" "$PEM"; do
    [[ ! -e $f && ! -L $f ]] || die "$f already exists; move it away first if you really mean to replace it"
done

# GNU date first (Linux), then BSD date (macOS).
stamp() { date -u -d "$1 days" +%Y%m%d%H%M%SZ 2>/dev/null || date -u -v"${1}d" +%Y%m%d%H%M%SZ; }
NOT_BEFORE=$(stamp -1)
NOT_AFTER=$(stamp +824)                    # 825 days after NOT_BEFORE, like api.example.com.pem

WORK=$(mktemp -d "$SIGNING/.mint.XXXXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
cat >"$WORK/leaf.cnf" <<EOF
[req]
distinguished_name = dn
prompt = no
[dn]
CN = $HOST
[v3_leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:$HOST
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF

# OpenSSL's chatter (signature ok, subject=...) only on failure. Keys go to files, never to stdout.
quiet() { local out; out=$("$@" 2>&1) || { printf '%s\n' "$out" >&2; die "failed: $1 $2"; }; }
quiet "$OPENSSL" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$WORK/key"
quiet "$OPENSSL" req -new -config "$WORK/leaf.cnf" -key "$WORK/key" -sha256 -out "$WORK/csr"
# A random 128-bit serial, so the root's serial file (bf-root-ca.srl) is left alone.
quiet "$OPENSSL" x509 -req -in "$WORK/csr" -CA "$CA" -CAkey "$CAKEY" -sha256 \
    -set_serial "0x$("$OPENSSL" rand -hex 16)" -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
    -extfile "$WORK/leaf.cnf" -extensions v3_leaf -out "$WORK/pem"

"$OPENSSL" verify -CAfile "$CA" -purpose sslserver -verify_hostname "$HOST" "$WORK/pem" >/dev/null \
    || die "the new certificate does not verify against $CA"
cmp -s <("$OPENSSL" pkey -in "$WORK/key" -pubout) <("$OPENSSL" x509 -in "$WORK/pem" -noout -pubkey) \
    || die 'the new key does not match the new certificate'

# ln refuses an existing name, so a file that appeared meanwhile is never overwritten.
ln -- "$WORK/key" "$KEY" || die "could not create $KEY"
ln -- "$WORK/pem" "$PEM" || { rm -f -- "$KEY"; die "could not create $PEM"; }
chmod 600 "$KEY" "$PEM"

echo "wrote $PEM and $KEY (0600; the key is not shown)"
"$OPENSSL" x509 -in "$PEM" -noout -subject -issuer -dates -ext subjectAltName -fingerprint -sha256
echo "chain: $("$OPENSSL" verify -CAfile "$CA" -purpose sslserver -verify_hostname "$HOST" "$PEM")"
