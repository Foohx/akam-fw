#!/bin/sh
# akam_api_key.sh -- on-camera tool for the keys of the local control API (port 8092; docs/porting/lsc-rotatable-2k/control-api.md).
# Run it over SSH. With no key installed the API port is CLOSED; the first admin key is created here and the running app opens the port by
# itself within about a second. It only calls the own app in its key-tool mode (`akam2k_rtsp --api-key ...`: no hardware, no thread, it only
# reads and rewrites /config/akam/api_keys, 0600, atomically); the app is the RAM copy the supervisor verified against BUILDINFO.
#
#   akam_api_key.sh new --scope read|write|admin --name LABEL    prints the key id and the 64-hex secret ONCE (stdout only)
#   akam_api_key.sh list                                         ids, names, scopes, created; never a secret
#   akam_api_key.sh revoke ID|NAME [--force]                     the last admin key is refused without --force
# Exit: 0 ok, 1 failure (no app, refused, write error), 2 usage, 4 the random pool is not ready (no key made).
# The secret is never put in argv, a log or a file other than the key file. Test hooks: AKAM_ROOT, AKAM_TMP_DIR, AKAM_APP, AKAM_APP_LIBS.

ROOT=${AKAM_ROOT:-}
TMPD=${AKAM_TMP_DIR:-$ROOT/tmp}
BIN=${AKAM_APP:-$TMPD/akam2k_rtsp}
LIBS=${AKAM_APP_LIBS:-$ROOT/appfs/lib}

usage() {
    echo "usage: akam_api_key.sh new --scope read|write|admin --name LABEL | list | revoke ID|NAME [--force]" >&2
    exit 2
}

case ${1:-} in
new | list | revoke) ;;
*) usage ;;
esac
[ -x "$BIN" ] || {
    echo "akam_api_key: $BIN not found: the own app is not running on this camera (see akam/akam2k.log)" >&2
    exit 1
}
umask 077
LD_LIBRARY_PATH=$LIBS exec "$BIN" --api-key "$@"
