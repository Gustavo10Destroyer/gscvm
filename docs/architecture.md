# Architecture

The VM is deliberately layered so that bytecode decoding, runtime semantics and
engine integration can be replaced independently.

```
T6 source (optional)               T6 bytecode
    |                                  |
    v                                  |
xsk::arc compiler + assembler          |
    |                                  |
    +---------------+------------------+
                    |
                    v
xsk::arc::t6::pc::context          upstream reader + disassembler + printer
                    |
                    v
gsc::t6::detail::load_module       internal IR: Module / Function / Instruction
                    |
                    v
gsc::t6::VM::Impl                  loader, resolver, scheduler, interpreter
    |        |             |
    |        |             +--> NativeRegistry / NativeContext
    |        +----------------> Host (dvars, logging, singletons, script IO)
    +-------------------------> Value / Array / Object
```

## Loading and compilation

`VM::load_module*` accepts either compiled T6 bytes or source text. Compiled
scripts are recognized by the `\x80GSC` header magic. Source is passed through
the upstream T6 compiler and assembler in-process, then loaded normally. The
script's directory is added to the search paths so `#include`d `.gsh` files and
source dependencies resolve relative to it. `VM::load_dependencies` compiles
source dependencies on demand as well.

## Upstream reuse

Only decoding is reused. `gsc-tool` already contains the authoritative T6 opcode
table (`t6_code.cpp`), the string/hash tables, the header parser, compiler and
disassembler. The loader calls that disassembler and converts its assembly view
into a small runtime IR, so the VM does not duplicate byte-level layout rules.

The upstream disassembler is patched at build time in one place to recognize
`OP_ProfileStart`; see [upstream-changes.md](upstream-changes.md).

## Engine boundary

The VM implements bytecode execution and cooperative scheduling, not the IW/T6
engine runtime. Engine services are supplied through `Host`, the native registry
and custom objects. Unregistered engine natives fail as unresolved imports.
Engine entities and `spawn()` are not ported, and there are no current plans to
implement them; `spawnstruct()` is only a generic VM object. See
[compatibility.md](compatibility.md) for the full scope and issue-reporting
guidance.

## Internal IR

`src/gsc/t6/module.hpp` defines:

- `Instruction`: opcode, operand strings, absolute offset, size, resolved jump
  target, and a parsed switch table.
- `Function`: name, namespace, parameter/flag bytes, instructions and an
  offset-to-index map.
- `Module`: functions, includes, disassembly text, aliases and lazy metadata.

Control flow is resolved once at load time. A jump to a non-instruction boundary
or a malformed switch table is a load error, never a runtime surprise.

## Values

`Value` is a tagged value with these types:

`undefined`, `boolean`, `integer`, `float`, `string`, `istring`, `hash`,
`vector`, `array`, `object`, `function`, `thread`, `animtree`, `animation`.

Strings are owned values. Arrays and objects are reference-like shared handles,
matching GSC assignment semantics. Arrays preserve insertion order for
`foreach`; the original engine uses hash order, so iteration order is explicitly
an approximation. Arrays are indexed with `[]`; `.size` is the only field-like
property they expose, and all other dot accesses are rejected. Objects are
polymorphic, allowing a host to expose engine entities without changing the VM.

`Value::to_string()` is the canonical serialization used by string concatenation
and console formatting. Built-in types have fixed representations; objects
override the virtual `Object::to_string()` hook, which is how native types and
C++ bindings control their textual form. `ScriptObject` uses this hook to emit a
detailed indented tree for levels and structs, including property names, value
types, nested arrays/objects, and cycle/depth guards. `format_message()`
implements the engine's `&&N` placeholders and `^0`..`^7` colour codes.

## Frames and locals

A frame holds a module, function, program counter, evaluation stack, locals and
the method receiver (`self`). Locals are stored in declaration order. GSC
bytecode indexes them in reverse declaration order, so the VM maps index `i` to
`locals[size - 1 - i]`.

`OP_SafeCreateLocalVariables` carries the declaration list. Parameters occupy
the first slots; remaining slots become undefined locals. Extra call arguments
are kept for an eventual `vararg` local.

## Calls

Imports carry a namespace, name, parameter count and flags. Resolution order is:

1. the current module,
2. the import namespace as a module,
3. the entry module,
4. every loaded module,
5. a module loaded from the host or search paths.

If no script function is found, the native registry is consulted. This is how
ordinary `println(...)`-style engine builtins work without a separate opcode.
Modules listed in a header's include section are loaded explicitly with
`load_dependencies()`; the CLI does this before invoking the entry point.

For method calls the receiver is pushed after the arguments and before the call
opcode. For pointer calls the function value is pushed last. The interpreter
pops in exactly that order, then pops the `OP_PreScriptCall` marker.

## Scheduler

Threads are cooperative. A thread is `ready`, `running`, `waiting_time`,
`waiting_frame`, `waiting_event`, `finished`, `failed` or `aborted`.

`run_until_idle()` repeatedly executes ready threads in creation order. `tick(dt)`
advances the logical clock, wakes timed/frame waiters, then runs ready threads.
`notify(target, event, args)` resumes matching `waittill` waiters and aborts
matching `endon` subscriptions. Event names are case-insensitive and
`waittillmatch` supports `*` patterns. Wait arguments are pushed in reverse
delivery order so the first `OP_SafeSetWaittillVariableFieldCached` consumes
argument 0. `endon` subscriptions are checked on every matching notification,
including when the target thread is ready, waiting on time or waiting on a
different event.

Registering an `endon` yields once when another thread is already runnable. This
lets instances spawned in the same scheduler pass register their handlers before
any of them enters its loop body, so the common `notify`/`endon` single-instance
idiom also works for same-frame spawns. `notify` itself remains immediate; a
thread aborted by it is never scheduled again.

Termination is thread-wide and absolute. A matching `endon` aborts the owning
thread even while it is executing a synchronous script call, function-pointer
call or native call. Pending wait and delivered-argument state is cleared, the
remaining synchronous frames are never resumed, and the thread cannot be
rescheduled.

`VM::call` starts the entry point as a thread and drives the whole VM until
every spawned thread has terminated. Timed and frame waits are advanced
automatically. If the remaining live threads can only wait on events that will
never be notified, the call raises a deadlock `RuntimeError` naming the waiting
thread, function and event instead of returning with live work behind it.
`run_until_complete()` provides the same drain for threads created with
`start()`. For externally driven events, use `start()`, `notify()` and
`run_until_idle()`/`tick()` explicitly.

`wait <seconds>` (and the equivalent `wait(seconds)` syntax) suspends only the
calling thread. The operand is a duration in seconds; integers and fractional
values are supported, so `wait 1`, `wait(2)` and `wait 0.05` all compile to the
same timed-wait opcode. A thread is stored as `waiting_time` with a logical
`wake_time`; ready threads continue running first, then the scheduler advances
the clock and resumes every thread whose deadline has expired. Manual `tick(dt)`
integration uses a small tolerance so host ticks like `0.1` wake a `wait 0.1`
thread reliably despite GSC's 32-bit float literals.

Automatic scheduling has two selectable modes through `VMConfig`:

- `ExecutionMode::real_time` advances directly to the nearest deadline. A
  `wait 0.01` therefore resumes at exactly `clock + 0.01`.
- `ExecutionMode::tick_based` advances in fixed steps of
  `1.0 / VMConfig::tick_rate`. Timed waits resume on the first tick boundary at
  or after their deadline, and `wait 0`/negative waits consume exactly one tick.
  Any positive finite tick rate is accepted, such as 20, 40 or 60 Hz.

`notify` and `endon` remain immediate within the current tick: a resumed waiter
is marked ready and runs in the same scheduler pass, before the next tick
advances. In both modes `VMConfig::real_time` controls wall-clock pacing. It is
enabled by default, so automatic advancement in `call()` and
`run_until_complete()` sleeps for the elapsed duration. Set it to `false` for
deterministic simulations where waits fast-forward as quickly as possible while
still respecting the selected mode's step boundaries. `tick(dt)` never sleeps
itself; the host supplies the elapsed time, and in tick-based mode that delta is
rounded up to a whole number of ticks. The same settings can be changed at
runtime with `set_execution_mode()`, `set_tick_rate()` and `set_real_time()`.

The instruction budget is per scheduling run and acts as runaway-loop
protection. Exhausting it raises an "infinite loop detected" `RuntimeError` with
the thread, module, function and instruction location. Recursive script calls
are bounded by `VMConfig::max_call_depth` (default 1024); exceeding it reports
the calling thread and function.

## Errors

Every execution error becomes a `RuntimeError` carrying module, function,
instruction offset, opcode name and thread id. Native code can raise the same
error through `NativeContext::error`. The scheduler never catches and hides
execution failures.
