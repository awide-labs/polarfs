#!/bin/bash
# Check if CHANGELOG.md needs to be updated based on Conventional Commits
#
# This script analyzes commits in a range to determine if they contain
# user-visible changes (feat, fix, or BREAKING CHANGE) and verifies that
# CHANGELOG.md has been updated accordingly.
#
# Usage: check_changelog.sh BASE_REF [CURRENT_REF] [CHANGELOG_FILE]
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)
#   CHANGELOG_FILE: Path to changelog file (optional, defaults to
#                   CHANGELOG.md)

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.ci/common.sh
source "${SCRIPT_DIR}/common.sh"

# Regex pattern for valid changelog entry ending
# Must end with (PROJ-NNNN) optionally followed by . or :
CHANGELOG_ENTRY_PATTERN='\([A-Z]+-[0-9]+\)[.:]\?$'

# Check changelog entry format for a specific commit
# Each paragraph starting with '- ' must end with a Jira issue reference
# Usage: check_changelog_entry_format COMMIT_SHA CHANGELOG_FILE
# Returns: 0 if valid, 1 if invalid (with errors printed)
check_changelog_entry_format() {
  local commit_sha="${1:-}"
  local changelog_file="${2:-CHANGELOG.md}"

  if [ -z "${commit_sha}" ]; then
    return 1
  fi

  # Get the diff for the changelog file in this commit (added lines only)
  local diff_output
  diff_output=$(git show --format="" --no-color "${commit_sha}" -- \
                "${changelog_file}" 2>/dev/null | \
                grep '^+' | grep -v '^+++' | sed 's/^+//' || true)

  if [ -z "${diff_output}" ]; then
    # No additions to changelog
    return 0
  fi

  # Process the diff to find paragraphs starting with '- '
  # A paragraph is a block starting with '- ' and continuing with indented
  # lines or sub-items (lines starting with '  ')
  local in_paragraph=false
  local current_paragraph=""
  local last_content_line=""
  local paragraph_start_line=""
  local invalid_entries=()
  local line_num=0

  while IFS= read -r line || [ -n "${line}" ]; do
    line_num=$((line_num + 1))

    # Check if this line starts a new top-level entry (dash at column 0)
    if [[ "${line}" =~ ^-\ .+ ]]; then
      # If we were in a paragraph, validate it before starting new one
      if [ "${in_paragraph}" = "true" ] && [ -n "${last_content_line}" ]; then
        if ! echo "${last_content_line}" | grep -qE '\([A-Z]+-[0-9]+\)[.:]?$'
        then
          invalid_entries+=("${paragraph_start_line}")
        fi
      fi

      # Start new paragraph
      in_paragraph=true
      current_paragraph="${line}"
      last_content_line="${line}"
      paragraph_start_line="${line}"

    elif [ "${in_paragraph}" = "true" ]; then
      # Check if this is a continuation (indented) or sub-item
      if [[ "${line}" =~ ^[[:space:]]+.+ ]]; then
        # Continuation or sub-item line
        current_paragraph+=$'\n'"${line}"
        # Only update last_content_line if this line has actual content
        # (not just whitespace)
        local trimmed
        trimmed=$(echo "${line}" | sed 's/^[[:space:]]*//')
        if [ -n "${trimmed}" ]; then
          last_content_line="${line}"
        fi
      elif [ -z "${line}" ]; then
        # Empty line - might end the paragraph or be between sub-items
        # Keep tracking but don't update last_content_line
        current_paragraph+=$'\n'"${line}"
      else
        # Non-indented, non-empty line that doesn't start with '-'
        # This ends the current paragraph
        if [ -n "${last_content_line}" ]; then
          if ! echo "${last_content_line}" | grep -qE '\([A-Z]+-[0-9]+\)[.:]?$'
          then
            invalid_entries+=("${paragraph_start_line}")
          fi
        fi
        in_paragraph=false
        current_paragraph=""
        last_content_line=""
        paragraph_start_line=""
      fi
    fi
  done <<< "${diff_output}"

  # Check the last paragraph if we're still in one
  if [ "${in_paragraph}" = "true" ] && [ -n "${last_content_line}" ]; then
    if ! echo "${last_content_line}" | grep -qE '\([A-Z]+-[0-9]+\)[.:]?$'; then
      invalid_entries+=("${paragraph_start_line}")
    fi
  fi

  # Report invalid entries
  if [ ${#invalid_entries[@]} -gt 0 ]; then
    log_error "    Invalid changelog entries (must end with Jira reference):"
    for entry in "${invalid_entries[@]}"; do
      # Truncate long entries for display
      local display_entry="${entry}"
      if [ ${#entry} -gt 60 ]; then
        display_entry="${entry:0:57}..."
      fi
      log_error "      → ${display_entry}"
    done
    return 1
  fi

  return 0
}

usage() {
  echo "Usage: $0 BASE_REF [CURRENT_REF] [CHANGELOG_FILE]"
  echo ""
  echo "  BASE_REF: Base commit/ref to compare against (required)"
  echo "  CURRENT_REF: Current commit/ref (optional, defaults to HEAD)"
  echo "  CHANGELOG_FILE: Path to changelog file (optional, defaults to"
  echo "                  CHANGELOG.md)"
  exit 1
}

main() {
  local base_ref="${1:-}"
  local current_ref="${2:-HEAD}"
  local changelog_file="${3:-CHANGELOG.md}"

  if [ -z "${base_ref}" ]; then
    log_error "Error: BASE_REF is required"
    echo ""
    usage
  fi

  echo "Base ref: ${base_ref}"
  echo "Current ref: ${current_ref}"
  echo "Changelog file: ${changelog_file}"
  echo ""

  # Get the list of commits in the range
  local commits
  commits=$(get_commits_in_range "${base_ref}" "${current_ref}")

  if [ -z "${commits}" ]; then
    log_warning "No commits found in range, skipping changelog check"
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

  echo "Analyzing commits for user-visible changes..."
  echo "---"

  local needs_changelog=false
  local commits_missing_changelog=()
  local commits_invalid_format=()

  local line
  while IFS= read -r line; do
    if [ -z "${line}" ]; then
      continue
    fi

    local commit_sha commit_msg
    commit_sha=$(echo "${line}" | cut -d' ' -f1)
    commit_msg=$(echo "${line}" | cut -d' ' -f2-)

    # Skip excluded commits (merge commits and their brought commits)
    if is_commit_excluded "${commit_sha}" "excluded_commits"; then
      if is_merge_commit "${commit_sha}"; then
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} (merge commit)"
      else
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                    "(brought by merge)"
      fi
      continue
    fi

    # Get full commit message body
    local full_msg
    full_msg=$(get_commit_body "${commit_sha}")

    # Check if commit explicitly skips changelog update
    # Uses Conventional Commits footer format: skip-changelog: true
    if echo "${full_msg}" | grep -qiE "^skip-changelog:\s*true"; then
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                  "(skip-changelog)"
      continue
    fi

    # Extract commit type from Conventional Commits format
    # Format: <type>[optional scope][optional !]: <description>
    local commit_type
    commit_type=$(extract_commit_type "${commit_msg}")

    # Check if this commit has user-visible changes requiring changelog
    # feat, fix, perf, or breaking changes require changelog updates
    local has_user_visible_changes=false
    local is_breaking=false

    # Check for breaking change indicator in type (type! or type(scope)!:)
    if echo "${commit_msg}" | grep -qE "${CC_HEADER_BREAKING}"; then
      has_user_visible_changes=true
      is_breaking=true
    fi

    # Check for BREAKING CHANGE in commit message body
    if echo "${full_msg}" | grep -qiE "^BREAKING CHANGE:|^BREAKING:"; then
      has_user_visible_changes=true
      is_breaking=true
    fi

    # Check for user-visible commit types
    case "${commit_type}" in
      feat|fix|perf)
        has_user_visible_changes=true
        ;;
    esac

    # If this commit has user-visible changes, check if changelog was updated
    if [ "${has_user_visible_changes}" = "true" ]; then
      needs_changelog=true

      # Check if changelog was modified in this specific commit
      local changelog_in_commit=false
      if git show --name-only --format="" "${commit_sha}" 2>/dev/null | \
         grep -q "^${changelog_file}$"; then
        changelog_in_commit=true
      fi

      # Determine display type for reporting
      local display_type="${commit_type}"
      if [ "${is_breaking}" = "true" ]; then
        display_type="BREAKING"
      fi

      if [ "${changelog_in_commit}" = "false" ]; then
        local missing_entry="${display_type}: ${commit_msg} "
        missing_entry+="(${commit_sha:0:8})"
        commits_missing_changelog+=("${missing_entry}")
        log_error "  ✗ ${commit_sha:0:8} - ${commit_msg} " \
                  "(missing ${changelog_file} update)"
      else
        # Changelog was updated, now check the format of entries
        if check_changelog_entry_format "${commit_sha}" "${changelog_file}"; then
          log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                      "(${display_type}, ${changelog_file} updated)"
        else
          local format_entry="${display_type}: ${commit_msg} "
          format_entry+="(${commit_sha:0:8})"
          commits_invalid_format+=("${format_entry}")
          log_error "  ✗ ${commit_sha:0:8} - ${commit_msg} " \
                    "(${changelog_file} format error)"
        fi
      fi
    else
      # Commit doesn't have user-visible changes, no changelog needed
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                  "(no changelog needed)"
    fi
  done <<< "${commits}"

  echo "---"

  # If no user-visible changes detected, no changelog update needed
  if [ "${needs_changelog}" = "false" ]; then
    log_success "✓ No user-visible changes detected. " \
                "Changelog update not required."
    exit 0
  fi

  # Check if all commits with user-visible changes have valid changelog updates
  local has_errors=false

  if [ ${#commits_missing_changelog[@]} -gt 0 ]; then
    has_errors=true
    echo ""
    log_error "✗ ${changelog_file} must be updated for the following commits:"
    echo ""
    local commit
    for commit in "${commits_missing_changelog[@]}"; do
      echo "  - ${commit}"
    done
  fi

  if [ ${#commits_invalid_format[@]} -gt 0 ]; then
    has_errors=true
    echo ""
    log_error "✗ ${changelog_file} entries have invalid format in:"
    echo ""
    local commit
    for commit in "${commits_invalid_format[@]}"; do
      echo "  - ${commit}"
    done
    echo ""
    echo "Each changelog entry starting with '- ' must end with a Jira"
    echo "issue reference in format (PROJ-NNNN), optionally followed by"
    echo ". or :"
    echo ""
    echo "Example: - Add new feature for parallel query (PROJ-1234)"
  fi

  if [ "${has_errors}" = "true" ]; then
    echo ""
    echo "Please update ${changelog_file} in the respective commits to " \
         "document these changes."
    echo "See https://keepachangelog.com/en/1.1.0/ for the changelog format."
    exit 1
  fi

  log_success "✓ All commits with user-visible changes have valid " \
              "${changelog_file} updates"
  exit 0
}

main "$@"
