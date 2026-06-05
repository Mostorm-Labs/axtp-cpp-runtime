#!/usr/bin/env bash
set -euo pipefail

clang_format="${CLANG_FORMAT_BIN:-clang-format}"

find \
  core \
  sdk \
  json-rpc \
  transports \
  tools \
  \( -path '*/build/*' -o -path '*/generated/*' -o -path '*/thirdparty/*' \) -prune -o \
  -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' \) \
  -print0 | xargs -0 "$clang_format" -i
