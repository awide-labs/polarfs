#!/usr/bin/env bash
# Verify that a pushed release tag matches POLAR_VERSION at the same commit.
# Used by GitHub Actions (GITHUB_REF_NAME) and Bitbucket Pipelines (BITBUCKET_TAG).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

TAG="${GITHUB_REF_NAME:-}"
if [[ -z "${TAG}" ]]; then
  TAG="${BITBUCKET_TAG:-}"
fi

if [[ -z "${TAG}" ]]; then
  echo "Error: set GITHUB_REF_NAME or BITBUCKET_TAG to the pushed tag" >&2
  exit 1
fi

if [[ ! -f POLAR_VERSION ]]; then
  echo "Error: POLAR_VERSION not found at repo root" >&2
  exit 1
fi

# shellcheck source=../POLAR_VERSION
source "${ROOT}/POLAR_VERSION"
FILE_VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}${VERSION_EXTRA:-}"

if [[ "${TAG}" == v* ]]; then
  TAG_VERSION="${TAG#v}"
else
  TAG_VERSION="${TAG}"
fi

if [[ "${TAG_VERSION}" != "${FILE_VERSION}" ]]; then
  echo "Error: tag '${TAG}' (expected version '${TAG_VERSION}') does not match" >&2
  echo "POLAR_VERSION (file reports '${FILE_VERSION}')." >&2
  exit 1
fi

echo "OK: tag '${TAG}' matches POLAR_VERSION (${FILE_VERSION})"
