#!/usr/bin/env bash
# Regenerate the vendored corsix/fast-crc32 variants in ../generated/.
#
# Run by hand whenever you want to bump the upstream pin or change the set
# of variants. Edit VARIANTS below if you want to add/remove entries; the
# CMakeLists.txt and dispatch.cpp need matching updates to actually use a
# new variant.

set -euo pipefail

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/corsix/fast-crc32.git}"
UPSTREAM_REF="${UPSTREAM_REF:-main}"

# Each entry: "<output-filename> <generator-args...>"
VARIANTS=(
  "sse_crc32c_v8s3x3.c             -i sse        -p crc32c -a v8s3x3"
  "avx512_crc32c_v8s3x4.c          -i avx512     -p crc32c -a v8s3x4"
  "neon_crc32c_v3s4x2e_v2.c        -i neon       -p crc32c -a v3s4x2e_v2"
  "neon_eor3_crc32c_v8s2x4e_s2x1.c -i neon_eor3  -p crc32c -a v8s2x4e_s2x1"
)

script_dir="$(cd "$(dirname "$0")" && pwd)"
out_dir="${script_dir}/../generated"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

echo "Cloning ${UPSTREAM_URL} (${UPSTREAM_REF}) into ${work}"
git clone --depth=1 --branch="${UPSTREAM_REF}" "${UPSTREAM_URL}" "${work}/src"
echo "Upstream commit: $(git -C "${work}/src" rev-parse HEAD)"

cc="${CC:-cc}"
"${cc}" -O2 -std=c99 -o "${work}/generate" "${work}/src/generate.c"

mkdir -p "${out_dir}"
for entry in "${VARIANTS[@]}"; do
  # shellcheck disable=SC2206
  parts=(${entry})
  out="${parts[0]}"
  args=("${parts[@]:1}")
  echo "Generating ${out}"
  "${work}/generate" "${args[@]}" > "${work}/${out}"
  # The generator embeds argv[0] in the banner; rewrite to a stable string
  # so the diff doesn't depend on the build path.
  sed -i 's|^/\* .*generate -i|/* fast-crc32-generate -i|' "${work}/${out}"
  cp "${work}/${out}" "${out_dir}/${out}"
done

# Refresh license files.
cp "${work}/src/LICENSE.md"      "${out_dir}/LICENSE.md"
cp "${work}/src/LICENSE.MIT.md"  "${out_dir}/LICENSE.MIT.md"
cp "${work}/src/LICENSE.zlib.md" "${out_dir}/LICENSE.zlib.md"

echo "Done. Review the diff under ${out_dir}/."
