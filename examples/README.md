# Examples

These scripts are compiled in-process by the command-line runner. Build the
project first, then run them from the repository root.

| Script | Demonstrates |
| --- | --- |
| [`basics.gsc`](basics.gsc) | Functions, arguments, return values, arrays, `foreach` and vectors. |
| [`threads.gsc`](threads.gsc) | Cooperative threads, `wait`, `waittill`, `notify` and the `level` object. |
| [`include.gsc`](include.gsc) | `#include` resolution through `examples/helpers/math.gsc`. |
| [`main.gsc`](main.gsc) | Reusable coroutine and symbol helpers under [`helpers/`](helpers/). |

```sh
./build/gscvm examples/basics.gsc
./build/gscvm examples/threads.gsc
./build/gscvm examples/include.gsc
./build/gscvm examples/main.gsc
```

The runner calls `main` when no function name is supplied. To call another
exported function, pass its name after the script:

```sh
./build/gscvm examples/basics.gsc add 3 4
```

Inspect a script without running it:

```sh
./build/gscvm --list examples/basics.gsc
./build/gscvm --disasm examples/basics.gsc
```

The `println` native understands the engine's `&&N` argument placeholders and
`^0`..`^7` colour codes. The examples avoid engine-specific natives so they run
with the default headless host.
