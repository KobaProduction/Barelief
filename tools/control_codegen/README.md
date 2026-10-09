# Offline control code generation

This developer-only tool is not invoked by configure, build, install, or firmware.
The runtime dependencies are pinned separately in their vendor manifests. The
tool manifest pins official protoc 36.2, the pure Python protobuf 7.36.2 wheel,
nanopb 0.4.9.2 generator inputs, and the verified Python 3.14.7 executable.
No pip installation, network access, or global toolchain change is needed.

Use the existing verified Python executable (or an identical copy):

```powershell
& C:/Python/Python314/python.exe -S tools/control_codegen/run.py --check-tools
& C:/Python/Python314/python.exe -S tools/control_codegen/run.py --output-dir libs/bdsp_protocol/generated
```

Only the assigned codec owner runs the second command during the implementation
slot. Schema and options are explicit inputs; unmatched options fail generation.
Generate twice into distinct scratch directories and compare both `.pb.c` and
`.pb.h` byte-for-byte before checking them in. Record schema/options/output hashes
with the codec evidence. The runner suppresses bytecode and uses a temporary
nanopb Python descriptor, keeping the pinned vendor sources unchanged.

Tool availability alone does not establish codec correctness or reproducibility
of the control outputs. Ordinary consumers use checked-in generated C sources.
