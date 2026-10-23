# Upstream changes

`upstream/` is a checkout of [`xensik/gsc-tool`](https://github.com/xensik/gsc-tool)
and retains its GPLv3 license. gscvm keeps behavioural changes to that tree as
small as possible.

## 1. `src/arc/disassembler.cpp`: recognize `OP_ProfileStart`

The upstream decoder has an operand-size entry for `OP_ProfileStart` but the
disassembly switch only handled `OP_ProfileStop`. T6 emits both as zero-operand
profiling markers. The `OP_ProfileStart` case is enabled alongside
`OP_ProfileStop` so valid T6 bytecode containing a profile start can be loaded.

The submodule itself stays pristine. During configuration, CMake reads the
upstream translation unit, replaces the commented case with the enabled case,
and writes the patched copy to
`build/generated/upstream/src/arc/disassembler.cpp`. That generated file is
compiled instead of the upstream original. If upstream later enables the case
itself, CMake detects that the marker is gone and uses the original source.

## 2. CMake integration

Upstream ships a premake5 build. The root `CMakeLists.txt` instead builds the
following from the vendored sources:

- `xsk_zlib` from `upstream/deps/zlib`
- `xsk_utils`, `xsk_arc`, `xsk_gsc` from `upstream/src`
- the upstream CLI target `gsc-tool`
- the Catch2 amalgamation and upstream test sources

A minimal `xsk/version.hpp` is generated for the upstream CLI, because premake
normally produces it.

## 3. GCC 14 compatibility flags

The upstream assembly structs contain a member named `opcode`:

```cpp
opcode opcode{};
```

GCC's C++20 "declaration changes meaning" rule diagnoses this as an error.
Rather than rewriting upstream headers, GNU builds compile the vendored targets
with `-fpermissive`. The `gscvm` library and upstream CLI/test targets also include
these headers, so they receive the same flag. This is a compiler-compatibility
workaround only; no runtime behaviour changes.

## 4. Tests

The upstream suite is included by default (`GSCVM_BUILD_UPSTREAM_TESTS=ON`) and
shares the `gscvm-tests` Catch2 binary with the VM suite. The upstream
`test/main.cpp` is excluded because gscvm supplies the Catch2 `main`, and the
VM tests are tagged `[gscvm]` so they can be run independently:

```sh
./build/gscvm-tests "[gscvm]"
```

The upstream decompiler/recompiler corpus is large; `ctest` therefore allows up
to 30 minutes for the combined binary.

## 5. Fixtures

No upstream test data is modified. gscvm adds precompiled T6 fixtures under
`test/data/t6`, generated from `test/fixtures/t6` with the upstream CLI. See the
README for the regeneration command.
