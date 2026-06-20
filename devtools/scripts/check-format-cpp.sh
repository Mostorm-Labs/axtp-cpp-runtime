#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
clang_format="${CLANG_FORMAT_BIN:-clang-format}"

find \
  "$root/include" \
  "$root/src" \
  "$root/tests" \
  "$root/tools" \
  \( -path '*/build/*' -o -path '*/generated/*' -o -path '*/third_party/*' \) -prune -o \
  -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' \) \
  -print0 | xargs -0 "$clang_format" --dry-run --Werror
