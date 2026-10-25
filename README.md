# gscvm

`gscvm` is a standalone C++20 virtual machine for **Call of Duty: Black Ops II
(T6)** GSC bytecode. It reuses the battle-tested T6 reader, disassembler and
opcode tables from [`xensik/gsc-tool`](https://github.com/xensik/gsc-tool) and
adds the missing piece: an interpreter, a cooperative scheduler, typed script
values, host services and a native-function API.

The project is intentionally headless. Game-specific behaviour is supplied by a
`Host` implementation; the VM never fabricates engine services.

> [!WARNING]
> **Experimental software, created entirely by artificial intelligence.** This
> project is not affiliated with, endorsed by, or supported by Activision,
> Treyarch, xensik, or the upstream `gsc-tool` maintainers. Expect incomplete
> engine compatibility and undocumented edge cases. Do not use it where failure
> or data loss would be unacceptable. The project is provided without warranty
> under the GNU GPLv3 only; see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

## Status

- T6 PC32 bytecode, server and client profiles.
- Module loading, dependency loading and disassembly.
- Script function and method calls, recursion, locals, arrays, objects and
  vectors.
- All 125 T6 opcodes are decoded. Two reserved/name-based opcodes and one
  legacy store opcode are rejected explicitly; see [docs/opcodes.md](docs/opcodes.md).
- Cooperative threads with automatic lifetime management, `wait`, `waittill`,
  `waittillmatch`, `notify`, `endon`, timed waits and frame waits. `endon`
  registration yields once to runnable siblings, so same-frame instances all
  register before any instance enters its body. An `endon` notification
  terminates the whole thread immediately, including synchronous call stacks.
- Runaway protection: configurable recursion depth (default 1024), instruction
  budgets that detect infinite loops, and deadlock diagnostics naming the
  waiting thread/function.
- Selectable real-time or fixed-step tick scheduling through
  `VMConfig::execution_mode` and an arbitrary `tick_rate`; pacing can be
  disabled with `VMConfig::real_time = false`.
- Per-VM native registry with typed argument access.
- Emscripten WebAssembly target with in-memory files/search paths, structured
  state inspection, memory accounting and instruction-level breakpoints/steps.
- ES module JavaScript SDK with automatic value marshalling, JavaScript
  natives, host service callbacks, cooperative threads and debugging helpers.
- GPL-3.0-only, matching the upstream project.

The implementation is not a byte-for-byte recreation of the shipped T6 engine,
and it does not port the engine runtime. Engine entities created by `spawn()` are
not implemented and there are no current plans to add them. Semantics that could
not be recovered from the compiler/decompiler are called out in
[docs/opcodes.md](docs/opcodes.md); see
[docs/compatibility.md](docs/compatibility.md) for the engine boundary and
issue-reporting guidance.

## Building

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`upstream/` is pinned as a submodule. The build generates a patched copy of the
upstream disassembler for `OP_ProfileStart` without modifying the submodule; see
[docs/upstream-changes.md](docs/upstream-changes.md).

The build produces:

- `build/libgscvm.a` - the VM library
- `build/gscvm` - the command line runner
- `build/gsc-tool` - the upstream compiler/disassembler, useful for fixtures
- `build/gscvm-tests` - Catch2 suite (VM tests are tagged `[gscvm]`)

GCC 14 needs permissive mode for the vendored upstream headers because
`xsk::arc::instruction` has a member named `opcode`. That is the only compiler
workaround; it is isolated to the vendored compatibility targets.

### WebAssembly

The browser target is a separate Emscripten build. It produces a modularized
`gscvm.js` loader and `gscvm.wasm`; the native CLI and tests are disabled in this
configuration. The result is static-hosting friendly (no backend, pthreads,
COOP/COEP headers or WebAssembly threads required).

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DGSCVM_BUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j
```

Copy `build-wasm/gscvm.js` and `build-wasm/gscvm.wasm` next to the playground.

The JavaScript SDK in [`sdk/`](sdk/) wraps the C API, value marshalling,
JavaScript natives, host callbacks, threads, inspection and debugging. It is an
ES module. Load the Emscripten loader as a classic script, then import the SDK:

```html
<script src="gscvm.js"></script>
<script type="module">
import { createVM } from './sdk/gscvm.mjs';

const vm = await createVM({
  moduleOptions: { locateFile: (file) => `./${file}` },
  host: {
    dvar(name) {
      return name === 'scr_xscale' ? 1.5 : undefined;
    }
  }
});

vm.registerNative('jsadd', (ctx, a, b) => a + b);
vm.loadSource('main', `
main() {
  println("hello &&1", 42);
  return jsadd(20, 22);
}
`);

const result = vm.call('main', 'main');
console.log(result.toJS(), vm.takeOutput());
vm.close();
</script>
```

In Node.js, load the Emscripten module with `createRequire` and pass it as
`factory` (or create it yourself and pass `module`). See
[`sdk/README.md`](sdk/README.md) and [`sdk/gscvm.d.ts`](sdk/gscvm.d.ts) for the
full API, including host objects, cooperative threads, globals, object access,
breakpoints and memory inspection.

The browser playground creates a fresh VM for every run and executes a
user-editable `bootstrap.js` before the selected script. Use it to register
natives and bind browser APIs:

```js
async function bootstrap({ vm, host }) {
  vm.registerNative('jsadd', (_ctx, a, b) => a + b);
  host.level = () => ({ name: 'playground' });
}
```

The bootstrap function may be async, for example to prefetch data with `fetch`,
but native callbacks must be synchronous. See the WebAssembly guide in the
documentation site for complete networking and localStorage examples.

The lower-level C API remains available for custom integrations. It uses UTF-8
strings and JSON results. Functions that return `char*` allocate memory owned by
the caller; release it with `Module._gscvm_free(ptr)`. Use
`Module.stringToUTF8`/`Module.lengthBytesUTF8` to create input strings and
`Module.UTF8ToString` to read results. A small helper for JSON-returning calls:

```js
function callJson(name, argTypes, args) {
  const ptr = Module.ccall(name, 'number', argTypes, args);
  if (!ptr) return null;
  const text = Module.UTF8ToString(ptr);
  Module._gscvm_free(ptr);
  return JSON.parse(text);
}

const loaded = callJson('gscvm_load_source', ['string', 'string'], ['scripts/main.gsc', source]);
callJson('gscvm_load_dependencies', ['string'], ['main']);
const run = callJson('gscvm_start', ['string', 'string'], ['main', 'main']);
```

The lower-level workflow is:

1. Add source/bytecode files with `gscvm_add_source(path, text)` or
   `gscvm_add_file(path, ptr, size)`. Paths use forward slashes.
2. Add one or more virtual search paths with `gscvm_add_search_path(path)`;
   `gscvm_clear_search_paths()` removes them. `gscvm_resolve_path(path)` returns
   the canonical virtual path (or null) and understands extensionless requests.
3. Load an entry file with `gscvm_load_file(path, name)`, or load bytes directly
   with `gscvm_load_module(name, ptr, size)`. Call `gscvm_load_dependencies(name)`
   before executing scripts that use `#include`.
4. Execute with `gscvm_call(module, function)` for a complete run, or
   `gscvm_start` + `gscvm_run_until_idle`/`gscvm_tick`/`gscvm_notify` for
   cooperative control. `gscvm_set_execution_mode(1)` selects fixed-step tick
   mode; `gscvm_set_tick_rate` changes the rate.
5. Read console output with `gscvm_take_output()`.

Inspection and debugging C API:

- `gscvm_threads()`, `gscvm_thread_info(id)`: thread status, waits, results,
  call frames, locals, varargs and stack values.
- `gscvm_globals()`: `level`, `game`, `world`, `classes` and `anim` with nested
  array/object values. Use `VM::inspect_value` from C++ when a host retains a
  `Value`.
- `gscvm_memory_stats()`: module/function/instruction counts, live threads,
  frames, locals, stack items, arrays, objects, total and nested value counts,
  and an estimated byte total.
- `gscvm_memory_blocks(previewBytes)` and `gscvm_module_bytes(name)`: logical
  memory regions (virtual files, modules, reachable arrays/objects) with hex
  previews, and full module bytecode as hex. Raw pointers are deliberately not
  exposed because WebAssembly linear-memory addresses are not stable.
- `gscvm_modules()` and `gscvm_disassembly(name)`: loaded module metadata and
  disassembly, useful for mapping editor breakpoints to instruction indices.
- `gscvm_set_breakpoint(module, function, pc)`: instruction-index breakpoint
  (0-based); `gscvm_clear_breakpoints()`, `gscvm_breakpoints()`.
- `gscvm_debug_state()`, `gscvm_step()`, `gscvm_resume()` and `gscvm_pause()`:
  pause at breakpoints, execute one instruction, resume to the next breakpoint,
  or request a pause before the next instruction.

The default WASM configuration uses virtual time (`real_time = false`). If the
playground needs paced execution, drive it from `requestAnimationFrame` with
`gscvm_tick(delta)` rather than enabling wall-clock sleeps in the browser.

## Command line

```sh
# Source scripts are compiled automatically; main is the default entry point
./build/gscvm path/to/script.gsc
./build/gscvm path/to/script.gsc add 3 4

# Compiled T6 bytecode works the same way
./build/gscvm path/to/compiled.gsc
./build/gscvm path/to/compiled.gsc main

# Inspect source or compiled scripts
./build/gscvm --info path/to/script.gsc
./build/gscvm --list path/to/script.gsc
./build/gscvm --disasm path/to/script.gsc
```

The runner detects compiled T6 headers by their magic. Anything else is treated
as source, compiled in-process, and then executed. Included `.gsh` files are
resolved from the script directory, the current directory, and `--search` paths.
If no function is named, `main` is called.

`gscvm` registers a few convenience natives (`print`, `println`, `iPrintLn`,
`iPrintLnBold`, `gettime`) and a `DefaultHost` with an in-memory dvar table.
`spawnstruct()` is provided by the VM itself and returns a dynamic field
container. `array(...)` builds an array from its arguments with zero-based
integer keys. The type predicates `isarray()`, `isstring()`, `isint()` and
`isfloat()` are also built in.

Arrays are indexed with `[]` only. `.size` is the sole dot-accessible property;
any other field access on an array is a runtime error.

`println` follows the engine's format-string convention. The first argument is
the format; `&&1`, `&&2`, ... are replaced by the following arguments. Colour
codes `^0`..`^7` are translated to ANSI, with `^7` resetting the colour. If a
colour is left active, the runner appends an ANSI reset before the newline.

## Embedding

```cpp
#include "gsc/gscvm.hpp"

using namespace gsc::t6;

VMConfig config;
config.host = std::make_shared<DefaultHost>();
config.search_paths.push_back("scripts");

// Optional fixed-step scheduling instead of exact real-time deadlines:
// config.execution_mode = ExecutionMode::tick_based;
// config.tick_rate = 20.0;

VM vm{ config };

vm.natives().register_function("", "distance", [](NativeContext& ctx) {
    auto const a = ctx.get_argument<Vector3>(0);
    auto const b = ctx.get_argument<Vector3>(1);
    auto const dx = a.x - b.x;
    auto const dy = a.y - b.y;
    auto const dz = a.z - b.z;
    ctx.set_return_value(Value::floating(std::sqrt(dx * dx + dy * dy + dz * dz)));
});

vm.load_module_file("scripts/example.gsc", "example");
vm.load_dependencies("example");

// `call` starts the entry point as a thread and drains every spawned thread.
// A child that can only wait for an event that is never notified is a deadlock.
auto result = vm.call("example", "main");

// Externally driven cooperative use:
auto thread = vm.start("example", "worker");
vm.run_until_idle();
vm.notify(Value::object(vm.level_object()), "wake");
vm.run_until_complete();
if (vm.thread_status(thread) == ThreadStatus::finished)
{
    auto value = vm.thread_result(thread);
}
```

Natives are copied into the VM by value. Arrays, objects and threads are shared
handles, so retaining a `Value` in a native keeps the underlying data alive.

### Inspection and breakpoints

The same inspection data is available directly from C++:

```cpp
auto const state = vm.debug_state();          // paused thread/instruction
auto const threads = vm.threads();            // ThreadInfo, including frames
auto const globals = vm.globals();            // VariableInfo trees
auto const stats = vm.memory_stats();         // counts and estimated bytes
auto const blocks = vm.memory_blocks(64);     // logical regions + hex preview

vm.set_breakpoint("example", "worker", 0);    // instruction index, 0-based
auto thread = vm.start("example", "worker");
vm.run_until_idle();                          // pauses at the breakpoint
vm.step();                                    // exactly one instruction
vm.resume();                                  // next breakpoint / blocked / done
```

Breakpoints are module/function/instruction-index based because T6 bytecode does
not carry a source-line map. The disassembly and `ModuleInfo::functions` expose
the function offsets; the playground can map its editor to instruction indices
using its own compiler metadata or function-entry breakpoints.

Virtual files are available to C++ embedders too:

```cpp
vm.add_search_path("scripts");
vm.add_virtual_file("scripts/helper.gsh", "helper_value() { return 77; }");
vm.add_virtual_file("scripts/main.gsc", "#include helper;\nmain() { return helper_value(); }");
vm.load_module_file("scripts/main.gsc", "main");
vm.load_dependencies("main");
```

Native objects can customize concatenation and console output by overriding
`Object::to_string()`. `Value::to_string()` is the serialization used by string
concatenation and `format_message()`; the built-in vector, function, thread,
animtree and animation values already have stable representations. Levels and
structs produce a detailed, indented tree of property names, value types and
values; nested arrays and structs are included, and cycles/depth are bounded.

## Fixtures

Tests use precompiled T6 binaries in `test/data/t6`. Their sources live in
`test/fixtures/t6` and can be regenerated with the upstream CLI:

```sh
for src in test/fixtures/t6/*.gsc; do
    ./build/gsc-tool -m comp -g t6 -s pc -i server -w test/fixtures/t6 "$src"
done
cp compiled/t6/*.gsc test/data/t6/
```

## Examples

Runnable scripts live in [`examples/`](examples/). Start with:

```sh
./build/gscvm examples/basics.gsc
./build/gscvm examples/threads.gsc
./build/gscvm examples/include.gsc
```

See [examples/README.md](examples/README.md) for the concepts each script
demonstrates.

## Documentation

- [Command-line runner](docs/cli.md)
- [Embedding the VM in C++](docs/embedding.md)
- [Extensibility and host integration](docs/extensibility.md)
- [Native C++ bindings](docs/native-bindings.md)
- [Architecture](docs/architecture.md)
- [Opcode coverage and semantics](docs/opcodes.md)
- [Upstream changes](docs/upstream-changes.md)

## License

gscvm is licensed under the **GNU General Public License v3.0 only**
(`GPL-3.0-only`). See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

The [`xensik/gsc-tool`](https://github.com/xensik/gsc-tool) submodule and its
dependencies retain their original copyrights and licenses. gscvm is an
independent experimental project and is not affiliated with or endorsed by the
upstream authors or the game publishers.
