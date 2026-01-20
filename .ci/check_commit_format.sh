#!/bin/bash
# Check if commits comply with Conventional Commits specification
#
# This script verifies that all commits in a given range follow the
# Conventional Commits specification format.
#
# Usage: check_commit_format.sh BASE_REF [CURRENT_REF]
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.ci/common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
  echo "Usage: $0 BASE_REF [CURRENT_REF]"
  echo ""
  echo "  BASE_REF: Base commit/ref to compare against (required)"
  echo "  CURRENT_REF: Current commit/ref (optional, defaults to HEAD)"
  exit 1
}

main() {
  local base_ref="${1:-}"
  local current_ref="${2:-HEAD}"

  if [ -z "${base_ref}" ]; then
    log_error "Error: BASE_REF is required"
    echo ""
    usage
  fi

  echo "Base ref: ${base_ref}"
  echo "Current ref: ${current_ref}"
  echo ""

  # Get the list of commits in the range
  local commits
  commits=$(get_commits_in_range "${base_ref}" "${current_ref}" "%H")

  if [ -z "${commits}" ]; then
    log_warning "No commits found in range, skipping check"
    exit 0
  fi

  # Build set of commits to exclude (merge commits and their brought commits)
  local excluded_commits=()
  local excluded_sha
  while IFS= read -r excluded_sha; do
    if [ -n "${excluded_sha}" ]; then
      excluded_commits+=("${excluded_sha}")
    fi
  done <<< "$(get_excluded_commits "${base_ref}" "${current_ref}")"

  echo "Checking commits for Conventional Commits compliance..."
  echo "---"

  local invalid_commits=()
  local commit_count=0

  local commit_sha
  while IFS= read -r commit_sha; do
    if [ -z "${commit_sha}" ]; then
      continue
    fi

    # Skip excluded commits (merge commits and their brought commits)
    if is_commit_excluded "${commit_sha}" "excluded_commits"; then
      local commit_msg
      commit_msg=$(git log -1 --format="%s" "${commit_sha}" 2>/dev/null)
      if is_merge_commit "${commit_sha}"; then
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} (merge commit)"
      else
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                    "(brought by merge)"
      fi
      continue
    fi

    commit_count=$((commit_count + 1))
    local commit_msg
    commit_msg=$(git log -1 --format="%s" "${commit_sha}")
    local full_msg
    full_msg=$(get_commit_body "${commit_sha}")

    # Check if commit message follows Conventional Commits format
    # Format: <type>[optional scope]: <description>
    # Types: feat, fix, docs, style, refactor, perf, test, build, ci, chore,
    #        revert
    # Scope is optional and in parentheses
    # Description must be present and not empty
    if ! echo "${commit_msg}" | grep -qE "${CC_HEADER_WITH_DESC}"; then
      invalid_commits+=("${commit_sha:0:8}: ${commit_msg}")
      log_error "  Invalid: ${commit_sha:0:8} - ${commit_msg}"
      continue
    fi

    # Extract type and check if it's valid
    local commit_type
    commit_type=$(extract_commit_type "${commit_msg}")
    local valid_types
    valid_types="feat fix docs style refactor perf test build ci chore revert"
    if ! echo "${valid_types}" | grep -qw "${commit_type}"; then
      invalid_commits+=("${commit_sha:0:8}: ${commit_msg}")
      log_error "  Invalid type '${commit_type}': ${commit_sha:0:8} - " \
                "${commit_msg}"
      continue
    fi

    # Check commit header (subject line) length (must be <= 72 chars)
    if [ ${#commit_msg} -gt 72 ]; then
      local header_error
      header_error="${commit_sha:0:8}: ${commit_msg} "
      header_error+="(header exceeds 72 chars: ${#commit_msg})"
      invalid_commits+=("$header_error")
      log_error "  Header too long (${#commit_msg} chars): " \
                "${commit_sha:0:8} - ${commit_msg}"
      continue
    fi

    # Check commit body lines length (each line must be <= 72 chars)
    # Skip empty lines and footer lines (like "Refs:")
    local body_line
    local line_num=0
    while IFS= read -r body_line; do
      line_num=$((line_num + 1))
      # Skip empty lines
      if [ -z "${body_line}" ]; then
        continue
      fi
      # Skip footer lines (any line matching token: value format)
      # Footer lines follow the pattern: token: value
      if echo "${body_line}" | grep -qE "^[A-Za-z-]+:\s+"; then
        continue
      fi
      # Check line length
      if [ ${#body_line} -gt 72 ]; then
        local body_error
        body_error="${commit_sha:0:8}: ${commit_msg} "
        body_error+="(body line ${line_num} exceeds 72 chars: ${#body_line})"
        invalid_commits+=("$body_error")
        log_error "  Body line ${line_num} too long (${#body_line} chars): " \
                  "${commit_sha:0:8} - ${body_line:0:50}..."
        continue 2
      fi
    done <<< "${full_msg}"

    # Check for mandatory Refs: footer (must be exactly one)
    local refs_lines
    refs_lines=$(echo "${full_msg}" | grep -iE "^Refs:\s+.+" || true)
    local refs_count=0
    if [ -n "${refs_lines}" ]; then
      refs_count=$(echo "${refs_lines}" | grep -c .)
    fi
    if [ "${refs_count}" -eq 0 ]; then
      local new_entry
      new_entry="${commit_sha:0:8}: ${commit_msg} (missing Refs: footer)"
      invalid_commits+=("$new_entry")
      log_error "  $new_entry"
      continue
    fi
    if [ "${refs_count}" -gt 1 ]; then
      local new_entry
      new_entry="${commit_sha:0:8}: ${commit_msg} "
      new_entry+="(multiple Refs: footers found, only one allowed)"
      invalid_commits+=("$new_entry")
      log_error "  $new_entry"
      continue
    fi

    # Check that Refs: value is a comma-separated list of words
    local refs_line
    refs_line=$(echo "${refs_lines}" | head -1)
    local refs_value
    refs_value=$(echo "${refs_line}" | sed -E 's/^[Rr]efs:\s+//')
    if ! echo "${refs_value}" | grep -qE "^[A-Za-z0-9_-]+(,\s*[A-Za-z0-9_-]+)*$"; then
      local new_entry
      new_entry="${commit_sha:0:8}: ${commit_msg} "
      new_entry+="(Refs: must be comma-separated list of words)"
      invalid_commits+=("$new_entry")
      log_error "  $new_entry"
      continue
    fi

    log_success "  ✓ ${commit_sha:0:8} - ${commit_msg}"
  done <<< "${commits}"

  echo "---"
  echo "Checked ${commit_count} commit(s)"

  # If there are invalid commits, report them and exit with error
  if [ ${#invalid_commits[@]} -gt 0 ]; then
    echo ""
    log_error "✗ ${#invalid_commits[@]} commit(s) do not comply with " \
              "Conventional Commits:"
    echo ""
    local commit
    for commit in "${invalid_commits[@]}"; do
      echo "  - ${commit}"
    done
    echo ""
    echo "Please ensure all commits follow the format:"
    echo "  <type>[optional scope]: <description>"
    echo ""
    echo "  [optional body]"
    echo ""
    echo "  Refs: <reference>[, <reference>...]"
    echo ""
    echo "Valid types: feat, fix, docs, style, refactor, perf, test, " \
         "build, ci, chore, revert"
    echo "The Refs: footer is mandatory and must be a comma-separated " \
         "list of issue references (e.g., 'PROJ-1234' or " \
         "'PROJ-1234, PROJ-5678')."
    echo "See https://www.conventionalcommits.org/ for details."
    exit 1
  fi

  log_success "✓ All commits comply with Conventional Commits specification"
  exit 0
}

main "$@"
