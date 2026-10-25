# gscvm JavaScript SDK

ES module wrapper for the gscvm WebAssembly build. It exposes module loading,
execution, cooperative threads, inspection/debugging, host services and
JavaScript native functions.

The SDK is ESM-only. It needs the Emscripten loader and binary produced by the
WASM build (`gscvm.js` and `gscvm.wasm`). This package does not bundle those
build artifacts.

## Build the loader

From the repository root:

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DGSCVM_BUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j
```

After building, run the SDK smoke test from the repository root:

```sh
node sdk/test/wasm.mjs
```

## Load the SDK

### Browser

Load the Emscripten loader as a classic script, then import the SDK module. The
loader defines `globalThis.createGscvmModule`.

```html
<script src="/playground/gscvm.js"></script>
<script type="module">
  import { createVM } from '/sdk/gscvm.mjs';

  const vm = await createVM({
    moduleOptions: {
      locateFile: (file) => `/playground/${file}`
    }
  });
</script>
```

### Node.js

```js
import { createRequire } from 'node:module';
import { createVM } from './sdk/gscvm.mjs';

const require = createRequire(import.meta.url);
const factory = require('./build-wasm/gscvm.js');

const vm = await createVM({ factory });
```

You can also create the Emscripten module yourself and pass `module`:

```js
const module = await createGscvmModule({ locateFile });
const vm = await createVM({ module });
```

## Quick start

```js
const vm = await createVM({
  factory,
  config: {
    executionMode: 'real_time',
    realTime: false
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
console.log(result.toJS());      // 42
console.log(vm.takeOutput());    // "hello 42\n"
vm.close();
```

If `gscvm.js` and `gscvm.wasm` are next to the HTML page, `locateFile` can be
omitted in the browser.

## Configuration

`createVM({ config })` accepts the same options as the C API:

```js
const vm = await createVM({
  factory,
  config: {
    instance: 'client',              // or 'server'
    executionMode: 'tick_based',     // or 'real_time'
    tickRate: 20,
    frameTime: 1 / 60,
    realTime: false,
    instructionLimit: 2_000_000,
    maxCallDepth: 512,
    maxStackSize: 65_536,
    maxThreads: 256,
    executeDevblocks: false,
    t6Fixup: false,
    searchPaths: ['scripts']
  }
});
```

Configuration can also be changed after creation with the corresponding
setters, for example `setInstructionLimit()`, `setMaxStackSize()`,
`setExecutionMode()` and `setDebugEnabled()`.

## Files and modules

```js
vm.addSource('scripts/helper.gsc', 'helper_value() { return 77; }');
vm.addSource('scripts/main.gsc', '#include helper; main() { return helper_value(); }');
vm.addSearchPath('scripts');

const info = vm.loadFile('scripts/main.gsc', 'main');
vm.loadDependencies('main');

console.log(info);
console.log(vm.modules());
console.log(vm.disassembly('main'));
```

`addFile()` and `loadModule()` accept a string, `ArrayBuffer`, or typed array.
`loadSource()` compiles source text directly and is usually simpler than
`addSource()` + `loadFile()`.

## Calls and threads

`call()` runs the entry thread and spawned threads to completion. For manual
scheduling use `start()`, `tick()`, `notify()` and the thread helpers:

```js
const thread = vm.start('main', 'worker');

vm.runUntilIdle();
vm.notify('wake');
vm.runUntilComplete();

console.log(thread.status());
console.log(thread.result().toJS());
```

`notify(event, ...args)` targets the level object.
`notifyTarget(target, event, ...args)` accepts a GSC value or a JavaScript value
such as an array/object.

## JavaScript natives

Natives receive a `NativeContext` followed by the decoded script arguments. The
return value is converted back to a GSC value. Unsupported values throw; async
callbacks are rejected.

```js
vm.registerNative('math', 'clamp', (ctx, value, low, high) =>
  Math.min(Math.max(value, low), high)
);

vm.registerNative('make_pair', (ctx, left, right) => ({
  left,
  right,
  label: 'pair'
}));
```

Namespaced natives are called as `space::name` from GSC. The T6 compiler records
the space as an include, but `loadDependencies()` recognizes registered native
spaces, so a missing `space.gsc` module is not treated as an error.

Including a native space makes its natives available unqualified. If several
included spaces define the same name, the first include wins; use `space::name`
to select an exact native.

Use `ctx.error('message')` to raise a script-visible runtime error. Enough
information is available to identify the call:

```js
vm.registerNative('debug_call', (ctx) => {
  console.log(ctx.space, ctx.name, ctx.threadId, ctx.argumentCount);
  console.log('receiver:', ctx.self);
});
```

Arrays, objects, vectors, functions, threads and the other GSC value kinds are
mapped automatically. Cyclic arrays/objects are supported via internal
references. Special GSC scalar types are represented as tagged objects such as
`{ $gsc: 'vector', x, y, z }` or `{ $gsc: 'istring', value }` in
`GscValue.toJS()` / callback arguments.

## Host services

Host callbacks supply engine services. They are synchronous.

```js
const host = {
  log(level, message) {
    console.log(`[${level}] ${message}`);
  },
  dvar(name) {
    if (name === 'scr_xscale') return 1.5;
    return undefined;
  },
  loadScript(name) {
    return null; // fall back to the VM search paths
  },
  level() {
    return { name: 'level', wave: 3 };
  },
  createObject(className) {
    return { class: className };
  }
};

const vm = await createVM({ factory, host });
```

Returned objects are bridged through the host so field access and mutation are
visible to GSC. `Map`, `Set`, plain objects and arrays are supported. When a
host dvar is absent (returns `null`/`undefined`), the VM falls back to its local
dvar table, which `setDvar()` updates.

## Values and inspection

```js
const value = vm.call('main', 'main');
console.log(value.type);       // GSC type name
console.log(value.display);    // GSC display string
console.log(value.toJS());     // decoded JavaScript value
console.log(value.raw);        // tagged wire value
```

`GscValue.fromJS(value)` encodes a JavaScript value and keeps its decoded form.
The module-level `encodeValue()` and `decodeValue()` functions expose the same
tagged protocol directly.

Inspection and debugging:

```js
vm.setDebugEnabled(true);
vm.setBreakpoint('main', 'worker', 0);

const thread = vm.start('main', 'worker');
vm.runUntilIdle();

console.log(vm.debugState());
console.log(vm.step());
console.log(vm.resume());
console.log(vm.threadInfo(thread.id));
console.log(vm.globals());
console.log(vm.memoryStats());
```

`globals()` returns the raw structured `level`, `game`, `world`, `classes` and
`anim` trees. Use `object('level')`, `objectGetField()`, `objectSetField()`,
`objectGetIndex()` and `objectKeys()` for direct access.

## API notes

- `R` functions with a `char*` result are wrapped for you; callers never deal
  with WebAssembly pointers.
- The loader is single-threaded. One `GscVM` per Emscripten module is the
  supported model.
- `close()` removes SDK callbacks and calls `gscvm_shutdown()`. A VM cannot be
  reused after closing.
- When script execution throws, the SDK throws a JavaScript `Error`. The message
  includes the module, function, opcode, offset and thread id where available.
- If your bundler needs a factory other than the global `createGscvmModule`,
  pass `options.factory` explicitly.

## License

GPL-3.0-only. See the repository `LICENSE` and `NOTICE.md`.
