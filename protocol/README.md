# HWLINK protocol schema

`hwlink.fbs` is the FlatBuffers schema for the HWLINK protocol
(SRS-GUI-F767 Appendix A).  It is the single source for the C headers in
`../src/generated` and should be shared unchanged with BOOT, HWVAL, the GUI
and the other demo applications.

The C headers were generated with flatcc 0.6.1:

```sh
flatcc -a -o src/generated protocol/hwlink.fbs
```

The generated files are committed so the `stm32-dev` container does not need
a host flatcc compiler.  When `flatcc` is on `PATH`, the CMake build
regenerates them into the build tree and uses those copies (LNK-012).  The
runtime is vendored in `../third_party/flatcc` under its original licence.

`Message.body` is an append-only union: add new members at the end and raise
the protocol minor version (LNK-013).  The file identifier is `HWLK`.
