#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
# Package a QStep release for manual upload (e.g. to a GitHub release):
#
#   tools/make-release.sh <version> [release-url]
#
# Produces release/<version>/:
#   qstep-<version>.tar.gz (+ .sha256)  prebuilt dist/ files, board/ scripts, configs/
#   qstep-bootstrap.sh                  step-2 installer with the version and URL filled in
# release-url defaults to the GitHub release download URL for the tag v<version>
# in andrewsharmon/qstep; set QSTEP_REPO=owner/repo (or pass the URL) for a fork.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${1:?usage: make-release.sh <version> [release-url]}
REPO=${QSTEP_REPO:-andrewsharmon/qstep}
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
cp -R board configs README.md NOTICE COPYING LICENSES "$STAGE/"
{
	echo "QStep $VER"
	echo "source: $(git rev-parse HEAD)"
	echo "base image: Arduino UNO Q Debian 20250807-136"
} > "$STAGE/VERSION"

# Reproducible-ish tarball: fixed owner, no macOS metadata/xattrs (Linux tar warns on them).
xattr -rc "$STAGE" 2>/dev/null || true
COPYFILE_DISABLE=1 tar -C "$STAGE" --no-xattrs --no-mac-metadata --uid 0 --gid 0 \
	--uname root --gname root -czf "$OUT/qstep-$VER.tar.gz" .
(cd "$OUT" && shasum -a 256 "qstep-$VER.tar.gz" > "qstep-$VER.tar.gz.sha256")

sed -e "s|@VERSION@|$VER|" -e "s|@RELEASE_URL@|$URL|" board/bootstrap.sh > "$OUT/qstep-bootstrap.sh"
chmod 755 "$OUT/qstep-bootstrap.sh"

ls -la "$OUT"
echo "Upload these three files to the release at: $URL"
