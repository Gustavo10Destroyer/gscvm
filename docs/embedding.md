# Embedding the VM in C++

`gscvm` is a static C++20 library. The public headers live under
`include/gsc/`; the umbrella header is `gsc/gscvm.hpp`.

## Link the library

With CMake, add this repository as a subdirectory and link the `gscvm` target:

```cmake
add_subdirectory(path/to/gscvm)
target_link_libraries(my_app PRIVATE gscvm)
```

The target publishes the public include directory and its required upstream
dependencies. A system install places headers under `include/gsc`, the library
under `lib`, and license/notice files under `share/doc/gscvm`.

## Create a VM

```cpp
#include <cmath>
#include <memory>

#include "gsc/gscvm.hpp"

using namespace gsc::t6;

int main()
{
    VMConfig config;
    config.host = std::make_shared<DefaultHost>();
    config.search_paths.push_back("scripts");

    // Optional deterministic scheduling:
    // config.execution_mode = ExecutionMode::tick_based;
    // config.tick_rate = 20.0;
    // config.real_time = false;

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

    auto result = vm.call("example", "main");
    return result.to_integer();
}
```

`VMConfig` is copied into the VM. Search paths can also be changed later with
`add_search_path()` and `clear_search_paths()`. The host is a shared pointer so
that it can be replaced with `set_host()`.

## Loading scripts

There are three entry points:

```cpp
auto info = vm.load_module_file("scripts/example.gsc", "example");
auto info2 = vm.load_module("example", bytecode);
```

`load_module_file()` accepts source or compiled bytes and detects compiled T6
headers by their `\x80GSC` magic. `load_module()` takes compiled bytes. To load
source text that does not exist on disk, add it as a virtual file and pass its
path to `load_module_file()`; the WASM API exposes the same operation as
`gscvm_load_source`.

After loading, call `load_dependencies(module)` before execution if the script
declares `#include` or `#using` dependencies. The loader records includes and
functions in `ModuleInfo`, exposes disassembly through `disassembly(name)`, and
retains the original bytes through `module_bytes(name)`.

## Virtual files

Virtual files are useful for editors, tests and WebAssembly. They take priority
over the host and filesystem loaders and are used for include resolution.

```cpp
vm.add_search_path("scripts");
vm.add_virtual_file("scripts/helper.gsh", "helper_value() { return 77; }");
vm.add_virtual_file("scripts/main.gsc",
    "#include helper;\nmain() { return helper_value(); }");

vm.load_module_file("scripts/main.gsc", "main");
vm.load_dependencies("main");
```

Paths use forward slashes. `resolve_virtual_path()` understands extensionless
include requests and returns the canonical in-memory path.
`read_virtual_file()`, `remove_virtual_file()`, `clear_virtual_files()` and
`virtual_files()` provide the rest of the in-memory file lifecycle.

## Calling and scheduling

`call()` runs the entry thread and every spawned thread to completion. It
advances timed and frame waits automatically. If the remaining threads can only
wait for events that will never be notified, it raises a deadlock
`RuntimeError`.

For externally driven events, use explicit scheduling:

```cpp
auto thread = vm.start("example", "worker");
vm.run_until_idle();

vm.notify(Value::object(vm.level_object()), "wake");
vm.run_until_complete();

if (vm.thread_status(thread) == ThreadStatus::finished)
    auto value = vm.thread_result(thread);
```

`tick(delta_seconds)` advances logical time, wakes timed/frame waiters and runs
until idle. In `ExecutionMode::tick_based`, the delta is rounded up to a whole
number of ticks. `tick()` never sleeps; wall-clock pacing only applies to
automatic advancement in `call()` and `run_until_complete()` when
`VMConfig::real_time` is true.

The scheduler status values are `ready`, `running`, `waiting_time`,
`waiting_frame`, `waiting_event`, `finished`, `failed`, `aborted` and `paused`.
Terminal threads remain inspectable until the VM is destroyed.

## Inspection and debugging

```cpp
auto const state = vm.debug_state();
auto const threads = vm.threads();
auto const globals = vm.globals();
auto const stats = vm.memory_stats();
auto const blocks = vm.memory_blocks(64);

vm.set_breakpoint("example", "worker", 0);
auto thread = vm.start("example", "worker");
vm.run_until_idle();   // pauses at the breakpoint
vm.step();             // exactly one instruction
vm.resume();           // next breakpoint, blocked state, or completion
```

Breakpoints use module, script function and zero-based instruction index
(`pc`). T6 bytecode does not carry a source-line map, so use the disassembly and
`ModuleInfo::functions` offsets to map editor positions. `set_debug_enabled()`,
`clear_breakpoints()`, `breakpoints()` and `pause()` complete the debugger API.

`thread_info()` and `threads()` return frames with locals, varargs, stack
values, wait state and terminal result/error. `globals()` returns `level`,
`game`, `world`, `classes` and `anim` as nested `VariableInfo` trees.
`memory_stats()` reports counts and an estimated byte total; `memory_blocks()`
returns logical regions with hex previews. Raw pointers are deliberately not
exposed because they are not stable across builds or WebAssembly memory growth.

## Errors

All library errors derive from `gsc::Error`. Execution failures use
`RuntimeError`, which carries the module, function, bytecode offset, opcode and
thread id:

```cpp
try
{
    vm.call("example", "main");
}
catch (RuntimeError const& error)
{
    std::cerr << error.what()
              << " at " << error.module() << "::" << error.function()
              << " +" << error.offset()
              << " (" << error.opcode() << ")"
              << " thread " << error.thread_id() << '\n';
}
```

The VM does not catch and hide execution failures. A native can raise the same
error through `NativeContext::error()`.

## Thread safety

A `VM` instance is not internally synchronized. Keep one VM per thread, or
serialize all access from the host. Different VM instances do not share native
registries, modules or runtime state.

See [Native C++ bindings](native-bindings.md) and
[Extensibility](extensibility.md) for host integration details.
