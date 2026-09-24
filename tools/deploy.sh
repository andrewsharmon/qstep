#!/bin/sh
# Deploy QStep from this Mac to an attached UNO Q over adb, using only the
# prebuilt files in dist/ (no kernel, firmware or HAL build needed).
#
#   tools/deploy.sh --dry-run --all     show what would be done
#   tools/deploy.sh --all               full install (see board/install.sh for steps)
#   tools/deploy.sh --hal --config      just some steps
#
# --packages needs internet on the board. This script sets up the adb reverse
# proxy tunnel (tinyproxy in the Lima VM on :3128) and sets the board clock,
# which has no RTC backup.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=$ROOT/tools/platform-tools/adb
STAGE=/root/qstep-stage

"$ADB" get-state >/dev/null || { echo "no board on adb"; exit 1; }
"$ADB" shell "date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')' >/dev/null"
"$ADB" reverse tcp:3128 tcp:3128 >/dev/null || true

(cd "$ROOT" && shasum -a 256 -c dist/SHA256SUMS) || { echo "dist/ checksum mismatch"; exit 1; }

"$ADB" shell "rm -rf $STAGE && mkdir -p $STAGE"
for d in dist board configs; do
	"$ADB" push "$ROOT/$d" "$STAGE/" >/dev/null
done
"$ADB" shell "cd $STAGE && sha256sum -c dist/SHA256SUMS --quiet && sh board/install.sh $*"
