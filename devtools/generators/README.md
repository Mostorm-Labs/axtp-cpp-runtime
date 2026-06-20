# AXTP C++ Runtime Generator

This generator is owned by `axtp-cpp-runtime`.

It consumes the AXTP spec checkout selected by `AXTP_SPEC_PATH`, or
`third_party/axtp-spec` when the environment variable is unset. The checkout must
match `AXTP_SPEC.lock.yaml`.

```bash
export AXTP_SPEC_PATH=/path/to/axtp
pnpm --dir devtools/generators install
pnpm --dir devtools/generators build
pnpm --dir devtools/generators test
pnpm --dir devtools/generators generate:runtime
```

Generated C++ artifacts are written to `include/core/protocol/generated/`.
