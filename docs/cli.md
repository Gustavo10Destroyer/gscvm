# Command-line runner

`gscvm` loads a T6 GSC source or compiled script, compiles source in-process,
and executes an exported function. The default entry point is `main`.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The runner is written to `build/gscvm`. The same build also produces the VM
library (`build/libgscvm.a`), the upstream compiler/disassembler
(`build/gsc-tool`), and the test binary (`build/gscvm-tests`).

## Usage

```text
gscvm [options] <script> [function [args...]]
```

`<script>` may be a `.gsc`/`.csc` source file or compiled T6 bytecode. Source is
compiled automatically. Arguments after the function name are converted from
text to GSC values: `true`/`false` become integers, quoted text becomes a
string, numeric text becomes an integer or float, and anything else becomes a
string.

### Options

| Option | Meaning |
| --- | --- |
| `--info` | Print module name, includes and function count. |
| `--list` | List exported functions with byte size and parameter count. |
| `--disasm` | Print the loaded module's disassembly. |
| `--search <path>` | Add a script/include search path. Repeatable. |
| `--client` | Use the client T6 profile instead of the server profile. |
| `--t6fixup` | Enable the upstream T6 header fixup. |
| `--devblocks` | Execute developer blocks instead of skipping them. |
| `--limit <n>` | Set the per-run instruction limit. `0` disables it. |
| `--tick-rate <hz>` | Select fixed-step scheduling at the given rate. |
| `--frames <n>` | Advance `n` scheduler ticks after the initial call. |
| `--time <seconds>` | Logical time advanced by each `--frames` tick. |
| `--notify <event> <target> [args...]` | Dispatch an event before the initial call. |
| `--help`, `-h` | Print usage. |

The runner always adds the current directory and the script's directory to the
search paths. `--search` paths are added after those.

## Examples

```sh
# Compile and run main()
./build/gscvm examples/basics.gsc

# Call another function with arguments
./build/gscvm examples/basics.gsc add 3 4

# Inspect a script without executing it
./build/gscvm --info examples/basics.gsc
./build/gscvm --list examples/basics.gsc
./build/gscvm --disasm examples/basics.gsc

# Add an include directory and use fixed-step scheduling
./build/gscvm --search scripts --tick-rate 20 scripts/main.gsc
```

`--info`, `--list` and `--disasm` are inspection modes. When one is present the
runner prints the requested data and exits without executing the script.

## Includes and dependencies

Source scripts may use `#include` and `#using`. The VM resolves them against the
current module's directory, the configured search paths, the host loader and the
in-memory virtual files. The runner loads the entry module's declared
dependencies before invoking the entry function. A dependency failure is printed
as a warning; execution then continues and may fail later if a required function
is missing.

The upstream `gsc-tool` is built for fixture generation:

```sh
./build/gsc-tool -m comp -g t6 -s pc -i server \
    -w test/fixtures/t6 test/fixtures/t6/basic.gsc
```

## Runtime behaviour

- `call()` and `run_until_complete()` advance timed and frame waits
  automatically.
- The runner registers `print`, `println`, `iprintln`, `iprintlnbold` and
  `gettime`.
- `println` expands `&&1`, `&&2`, ... placeholders and `^0`..`^7` colour codes.
- `spawnstruct()`, `array(...)`, `isarray()`, `isstring()`, `isint()` and
  `isfloat()` are provided by the VM or its built-in natives.
- A deadlock (for example a thread waiting on an event that is never notified)
  raises a runtime error with the waiting thread, function and event.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success, including inspection-only runs. |
| `1` | General load/compile/configuration error. |
| `2` | Missing script path or invalid command line. |
| `3` | GSC runtime error, including deadlock and runaway execution. |

Errors are written to stderr. Successful function results are printed as
`<function>: <value>`.
