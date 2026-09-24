#!/bin/sh
# Package an ArduCNC release for manual upload (e.g. to a GitHub release):
#
#   tools/make-release.sh <version> [release-url]
#
# Produces release/<version>/:
#   arducnc-<version>.tar.gz (+ .sha256)  prebuilt dist/ files, board/ scripts, configs/
#   arducnc-bootstrap.sh                  step-2 installer with the version and URL filled in
# release-url defaults to the GitHub release download URL for the tag v<version>;
# set ARDUCNC_REPO=owner/repo (or pass the URL) once the repo has a home.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${1:?usage: make-release.sh <version> [release-url]}
REPO=${ARDUCNC_REPO:-OWNER/arducnc}
URL=${2:-https://github.com/$REPO/releases/download/v$VER}
OUT=$ROOT/release/$VER

cd "$ROOT"
shasum -a 256 -c dist/SHA256SUMS >/dev/null
if [ -n "$(git status --porcelain -- dist board configs)" ]; then
	echo "dist/, board/ or configs/ have uncommitted changes; commit first so the release is traceable"
	exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/dist"
cp -R dist/kernel dist/hal dist/firmware dist/SHA256SUMS dist/README.md "$STAGE/dist/"
cp -R board configs README.md "$STAGE/"
{
	echo "ArduCNC $VER"
	echo "source: $(git rev-parse HEAD)"
	echo "base image: Arduino UNO Q Debian 20250807-136"
} > "$STAGE/VERSION"

# Reproducible-ish tarball: fixed owner, no macOS metadata.
COPYFILE_DISABLE=1 tar -C "$STAGE" --uid 0 --gid 0 --uname root --gname root -czf "$OUT/arducnc-$VER.tar.gz" .
(cd "$OUT" && shasum -a 256 "arducnc-$VER.tar.gz" > "arducnc-$VER.tar.gz.sha256")

sed -e "s|@VERSION@|$VER|" -e "s|@RELEASE_URL@|$URL|" board/bootstrap.sh > "$OUT/arducnc-bootstrap.sh"
chmod 755 "$OUT/arducnc-bootstrap.sh"

ls -la "$OUT"
echo "Upload these three files to the release at: $URL"
