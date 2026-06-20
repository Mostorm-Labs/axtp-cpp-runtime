#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 X.Y.Z[.R]" >&2
  exit 2
fi

version="$1"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
  echo "Runtime/tool version must match MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH.REVISION" >&2
  exit 2
fi

IFS=. read -r version_major version_minor version_patch version_revision_extra <<< "$version"
spec_version="$version_major.$version_minor.$version_patch"
runtime_revision="${version_revision_extra:-0}"
ecosystem_version="$spec_version"
if [[ "$runtime_revision" != "0" ]]; then
  ecosystem_version="$spec_version-runtime.$runtime_revision"
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runtime_name="$(basename "$root")"

write_version_file() {
  printf '%s\n' "$version" > "$1"
}

has_cmake_project_version() {
  local file="$1"
  [[ -f "$file" ]] || return 1
  perl -0ne 'if (/project\s*\([^)]*?\bVERSION\s+[0-9]+\.[0-9]+\.[0-9][^\s)]*/is) { $found = 1 } END { exit($found ? 0 : 1) }' "$file"
}

set_cmake_project_version() {
  local file="$1"
  VERSION="$version" perl -0pi -e 's/(project\s*\([^)]*?\bVERSION\s+)[0-9]+\.[0-9]+\.[0-9][^\s)]*/$1$ENV{VERSION}/is' "$file"
}

set_package_json_version() {
  local file="$1"
  local value="${2:-$version}"
  PACKAGE_JSON="$file" VERSION="$value" node --input-type=module <<'NODE'
import { readFileSync, writeFileSync } from "node:fs";

const file = process.env.PACKAGE_JSON;
const version = process.env.VERSION;
const data = JSON.parse(readFileSync(file, "utf8"));
data.version = version;
writeFileSync(file, `${JSON.stringify(data, null, 2)}\n`);
NODE
}

set_pyproject_version() {
  local file="$1"
  VERSION="$version" perl -0pi -e 's/(\[project\][\s\S]*?^version\s*=\s*")[^"]+(")/$1$ENV{VERSION}$2/m' "$file"
}

case "$runtime_name" in
  axtp-c-runtime)
    write_version_file "$root/VERSION"
    if has_cmake_project_version "$root/CMakeLists.txt"; then
      set_cmake_project_version "$root/CMakeLists.txt"
    fi
    ;;
  axtp-cpp-runtime)
    updated=false
    if has_cmake_project_version "$root/CMakeLists.txt"; then
      set_cmake_project_version "$root/CMakeLists.txt"
      updated=true
    fi
    if [[ -f "$root/VERSION" || "$updated" == "false" ]]; then
      write_version_file "$root/VERSION"
    fi
    ;;
  axtp-flutter-runtime)
    write_version_file "$root/VERSION"
    VERSION="$ecosystem_version" perl -0pi -e 's/^version:\s*[^\n]+/version: $ENV{VERSION}/m' "$root/pubspec.yaml"
    ;;
  axtp-python-runtime)
    if [[ -f "$root/pyproject.toml" ]]; then
      set_pyproject_version "$root/pyproject.toml"
    else
      write_version_file "$root/VERSION"
    fi
    if [[ -f "$root/setup.py" ]]; then
      VERSION="$version" perl -0pi -e 's/(version\s*=\s*")[^"]+(")/$1$ENV{VERSION}$2/' "$root/setup.py"
    fi
    ;;
  axtp-ts-runtime)
    write_version_file "$root/VERSION"
    set_package_json_version "$root/package.json" "$ecosystem_version"
    ;;
  axtp-mock-server)
    if [[ -f "$root/package.json" ]]; then
      set_package_json_version "$root/package.json"
    fi
    if [[ -f "$root/pyproject.toml" ]]; then
      set_pyproject_version "$root/pyproject.toml"
    fi
    if [[ -f "$root/VERSION" || ! -f "$root/package.json" && ! -f "$root/pyproject.toml" ]]; then
      write_version_file "$root/VERSION"
    fi
    ;;
  *)
    echo "Unsupported runtime/tool repository: $runtime_name" >&2
    exit 2
    ;;
esac

actual="$("$root/devtools/scripts/get-runtime-version.sh")"
if [[ "$actual" != "$version" ]]; then
  echo "Runtime/tool version update failed: expected $version, got $actual" >&2
  exit 1
fi

echo "Updated $runtime_name version to $version"
