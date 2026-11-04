# flatcc 0.6.1 runtime

This directory contains the flatcc 0.6.1 runtime (`src/runtime/builder.c`,
`verifier.c`, `emitter.c`, `refmap.c`) and its public headers from
<https://github.com/dvidelabs/flatcc>, used as released under the Apache
License 2.0 (see `LICENSE` and `NOTICE`).

Only the C runtime is built. The schema compiler is a host tool: if
`flatcc` is on `PATH`, the project CMake build regenerates
`src/generated` into the build tree; otherwise the committed generated
headers are used.
