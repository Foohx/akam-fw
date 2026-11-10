#!/usr/bin/env bash
# Host test of src/ak_rtsp/sha1.c and b64.c (ASan/UBSan): FIPS 180-1 vectors incl. the million-'a'
# one, padding boundaries, RFC 4648 vectors, strict base64 decoding. When openssl is installed the
# digests are also cross-checked against it on pseudo-random inputs. Needs a host C compiler;
# touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$REPO/src/ak_rtsp" \
    -o "$WORK/sha1_test" \
    "$REPO/tests/native/sha1_test.c" "$REPO/src/ak_rtsp/sha1.c" "$REPO/src/ak_rtsp/b64.c"
"$WORK/sha1_test"

# Cross-check with openssl on a few sizes of random data (optional).
if command -v openssl >/dev/null 2>&1; then
    cat >"$WORK/sum.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "sha1.h"
#include "b64.h"
int main(void)
{
    static unsigned char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    uint8_t d[20];
    char hex[41], enc[B64_ENCODED_LEN(sizeof(buf)) + 1];
    sha1(buf, n, d);
    for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    b64_encode(buf, n, enc, sizeof(enc));
    printf("%s %s\n", hex, enc);
    return 0;
}
EOF
    "$CC" -std=gnu99 -Wall -Wextra -Werror -I"$REPO/src/ak_rtsp" -o "$WORK/sum" "$WORK/sum.c" \
        "$REPO/src/ak_rtsp/sha1.c" "$REPO/src/ak_rtsp/b64.c"
    bad=0
    for n in 0 1 2 3 20 55 56 63 64 65 1000 4097 65536; do
        head -c "$n" /dev/urandom >"$WORK/data"
        want_sha=$(openssl dgst -sha1 <"$WORK/data" | sed 's/.*= //')
        want_b64=$(openssl base64 -A <"$WORK/data")
        got=$("$WORK/sum" <"$WORK/data")
        if [ "$got" != "$want_sha $want_b64" ]; then
            echo "  FAIL - openssl cross-check with $n random bytes"
            bad=1
        fi
    done
    if [ "$bad" = 0 ]; then echo "  ok   - SHA-1 and base64 agree with openssl on 13 random inputs"; else exit 1; fi
else
    echo "skip: openssl not installed, no cross-check"
fi
