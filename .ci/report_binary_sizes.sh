#!/usr/bin/env bash
# Print sizes of core PolarFS artifacts (for CI baselines, e.g. before/after LTO).
# Expects repo root as cwd; run after autobuild.sh.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

human() {
  if command -v numfmt >/dev/null 2>&1; then
    numfmt --to=iec --suffix=B "$1"
  else
    echo "$1"
  fi
}

# path:label
declare -a entries=(
  "bin/pfsdaemon:pfsdaemon"
  "lib/libpfsd.a:libpfsd"
  "lib/libpfs.a:libpfs"
)

total=0
echo "=== PolarFS artifact sizes ==="
for pl in "${entries[@]}"; do
  path="${pl%%:*}"
  label="${pl##*:}"
  if [[ ! -f "${path}" ]]; then
    echo "ERROR: ${path} not found. Run ./autobuild.sh first." >&2
    exit 1
  fi
  bytes=$(wc -c <"${path}" | tr -d ' ')
  total=$((total + bytes))
  printf "  %-10s  %-30s  %12s bytes  (~%s)\n" "${label}" "${path}" "${bytes}" "$(human "${bytes}")"
done
printf "  %-10s  %-30s  %12s bytes  (~%s)\n" "total" "(pfsdaemon+libpfsd+libpfs)" "${total}" "$(human "${total}")"
echo "================================"
