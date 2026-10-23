# Engine compatibility

`gscvm` executes **T6 GSC bytecode**. It is not a reimplementation of the
Call of Duty: Black Ops II engine, and it does not reproduce the engine's
runtime state.

## What the VM provides

- T6 source compilation and bytecode decoding through the pinned `gsc-tool`
  submodule.
- An interpreter for the T6 opcode set, with documented exceptions in
  [opcodes.md](opcodes.md).
- Typed values, arrays, objects, cooperative threads, waits, events and
  `endon`/`notify` scheduling.
- A `Host` interface and a per-VM native registry for application-specific
  services.
- Script loading, virtual files, dependency loading and inspection/debugging
  APIs.

## What it does not provide

`gscvm` does not include the IW/T6 engine runtime. In particular, it does not
provide:

- engine entity creation or the engine entity class hierarchy;
- AI, navigation, physics, collision, weapons, damage, perks, power-ups or
  game-mode logic;
- asset, animation, FX, sound, UI, networking or matchmaking services;
- the full engine native function set;
- the engine's dvar table, level lifecycle or save-game state.

A script that calls an engine function not registered by the host or CLI will
fail with an unresolved native or unresolved script function error. That is a
missing host/runtime service, not a VM opcode bug.

T6 source dependencies use `#include`. In `gsc-tool`, `#using`, `#insert`,
`#precache` and `#namespace` are size64 (T7) directives and are not part of the
T6 profile. `#using_animtree` and `#inline` are supported by the T6
preprocessor.

## Entities and `spawn()`

The engine's `spawn()` function and the entity objects it creates have **not
been ported**. There are no current plans to implement engine entities.
Faithful reproduction depends on proprietary engine behaviour, asset state,
physics, AI and networking, so a partial entity model would be misleading.

`spawnstruct()` is different: it creates a generic dynamic script object owned
by the VM. It is useful for plain data, but it is not an engine entity and has
no engine fields or behaviour.

A host can model application-specific objects with `Host::create_object()` and
custom `Object` subclasses, or register its own `spawn` native. See
[Extensibility](extensibility.md) and [Native bindings](native-bindings.md).
Such objects are host-defined; they are not the T6 entity system.

## When behaviour differs

The VM is not a byte-for-byte recreation of the shipped engine. Known
approximations are documented in [opcodes.md](opcodes.md) and the architecture
guide. In general, differences can come from:

- missing engine natives or host services;
- engine state that the VM does not simulate;
- undefined or engine-specific behaviour in original scripts;
- deliberate scheduler, ordering or value-model approximations.

If a script behaves differently from the engine, first check whether it depends
on an engine service that is not registered. Try a minimal script that isolates
the operation, and compare it with the same operation under the engine.

## Reporting a discrepancy

Open an issue in the repository's issue tracker. Include enough information to
reproduce the problem:

1. A minimal `.gsc`/`.csc` source file, or the compiled bytecode, that
   demonstrates the problem.
2. The exact command used to run it.
3. The actual output, exit code or error. For runtime errors, include the
   module, function, instruction offset, opcode and thread id when available.
4. The expected result and why you expect it.
5. Any relevant `Host` or native registrations, `VMConfig` settings, search
   paths and virtual files.
6. Build type, compiler/version, operating system and whether the native or
   WebAssembly build was used.

Please do not attach copyrighted game assets or proprietary scripts that you
cannot share. A reduced, self-contained reproduction is more useful than a full
map script.

## Minimal reproduction example

This script is syntactically valid T6 GSC, but it will not run unless the host
registers a `spawn` native:

```gsc
main()
{
    entity = spawn( "script_origin", ( 0, 0, 0 ) );
    entity thread monitor();
}

monitor()
{
    self endon( "death" );
    self waittill( "work", value );
    self notify( "ready", value );
}
```

The missing piece is the engine's `spawn()` implementation, not the
`thread`/`waittill`/`notify` scheduler.
