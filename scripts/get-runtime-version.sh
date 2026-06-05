#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_name="$(basename "$root")"

read_cmake_project_version() {
  local file="$1"
  [[ -f "$file" ]] || return 1
  perl -0ne 'if (/project\s*\([^)]*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9][^\s)]*)/is) { print "$1\n"; $found = 1 } END { exit($found ? 0 : 1) }' "$file"
}

case "$runtime_name" in
  axtp-c-runtime)
    if [[ -f "$root/VERSION" ]]; then
      tr -d '[:space:]' < "$root/VERSION"
      echo
    else
      read_cmake_project_version "$root/CMakeLists.txt"
    fi
    ;;
  axtp-cpp-runtime)
    if read_cmake_project_version "$root/CMakeLists.txt" 2>/dev/null; then
      :
    elif read_cmake_project_version "$root/core/CMakeLists.txt" 2>/dev/null; then
      :
    else
      tr -d '[:space:]' < "$root/VERSION"
      echo
    fi
    ;;
  axtp-flutter-runtime)
    awk '/^version:[[:space:]]*/ { sub(/^version:[[:space:]]*/, ""); print; found = 1; exit } END { if (!found) exit 1 }' "$root/pubspec.yaml"
    ;;
  axtp-python-runtime)
    awk 'in_project && /^version[[:space:]]*=/ { value = $0; sub(/^version[[:space:]]*=[[:space:]]*"/, "", value); sub(/".*$/, "", value); print value; found = 1; exit } /^\[project\]/ { in_project = 1 } /^\[/ && !/^\[project\]/ { in_project = 0 } END { if (!found) exit 1 }' "$root/pyproject.toml"
    ;;
  axtp-ts-runtime)
    node -e "console.log(JSON.parse(require('fs').readFileSync('$root/package.json', 'utf8')).version)"
    ;;
  axtp-mock-server)
    if [[ -f "$root/package.json" ]]; then
      node -e "console.log(JSON.parse(require('fs').readFileSync('$root/package.json', 'utf8')).version)"
    elif [[ -f "$root/pyproject.toml" ]]; then
      awk 'in_project && /^version[[:space:]]*=/ { value = $0; sub(/^version[[:space:]]*=[[:space:]]*"/, "", value); sub(/".*$/, "", value); print value; found = 1; exit } /^\[project\]/ { in_project = 1 } /^\[/ && !/^\[project\]/ { in_project = 0 } END { if (!found) exit 1 }' "$root/pyproject.toml"
    else
      tr -d '[:space:]' < "$root/VERSION"
      echo
    fi
    ;;
  *)
    echo "Unsupported runtime/tool repository: $runtime_name" >&2
    exit 2
    ;;
esac
