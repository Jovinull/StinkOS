#!/usr/bin/env bash
# Pull the official Freedoom + FreeDM 0.13.0 WAD assets from GitHub and drop
# them under wads/ for the build to bundle into the disk image. Idempotent --
# already-downloaded WADs are skipped on subsequent runs.
#
# Usage:
#   bash tools/fetch-wads.sh
set -euo pipefail

VERSION="0.13.0"
BASE="https://github.com/freedoom/freedoom/releases/download/v${VERSION}"

# Run from the project root so 'wads/' lands where the Makefile expects it.
cd "$(dirname "$0")/.."
mkdir -p wads

# fetch_zip <archive-name> <wad-name> [<wad-name> ...]
#   Downloads the release zip if any of the requested WADs are missing,
#   then extracts only those WADs (flatten the inner directory).
fetch_zip() {
	local archive="$1"; shift
	local need=0
	for wad in "$@"; do
		if [ ! -f "wads/${wad}" ]; then need=1; break; fi
	done
	if [ "${need}" -eq 0 ]; then
		echo "fetch-wads: $* already present, skipping ${archive}"
		return 0
	fi

	echo "fetch-wads: downloading ${archive}"
	curl -fL --progress-bar -o "wads/${archive}" "${BASE}/${archive}"
	for wad in "$@"; do
		echo "fetch-wads: extracting ${wad}"
		# -j strips the wrapping freedoom-0.13.0/ directory inside the zip.
		unzip -j -o "wads/${archive}" "*/${wad}" -d wads
	done
	rm "wads/${archive}"
}

fetch_zip "freedoom-${VERSION}.zip"  freedoom1.wad  freedoom2.wad
fetch_zip "freedm-${VERSION}.zip"    freedm.wad

echo
echo "fetch-wads: ready"
ls -lh wads/*.wad
