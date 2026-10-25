import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const require = createRequire(import.meta.url);
const factory = require(join(root, 'build-wasm', 'gscvm.js'));
const { createVM } = await import(pathToFileURL(join(root, 'sdk', 'gscvm.mjs')));

const level = { name: 'test_level', hits: 0 };
const host = {
  dvar(name) {
    return name === 'js_dvar' ? 3 : undefined;
  },
  level() {
    return level;
  }
};

const vm = await createVM({
  factory,
  moduleOptions: { locateFile: (file) => join(root, 'build-wasm', file) },
  host,
  config: { realTime: false, instructionLimit: 1_000_000 }
});

try {
  vm.registerNative('jsadd', (_ctx, a, b) => a + b);
  vm.registerNative('jscycle', () => {
    const value = { name: 'cycle' };
    value.self = value;
    return value;
  });

  vm.loadSource('main', `
main() {
  println("add=&&1", jsadd(20, 22));
  level.hits = level.hits + 1;
  return jsadd(1, 2);
}
cycle() { return jscycle(); }
dvar() { return getdvar("js_dvar"); }
local() { return getdvar("local_dvar"); }
worker() { wait 1; return jsadd(2, 3); }
`);

  assert.equal(vm.call('main', 'main').toJS(), 3);
  assert.equal(level.hits, 1);
  assert.match(vm.takeOutput(), /add=42/);

  const cycle = vm.call('main', 'cycle').toJS();
  assert.equal(cycle.self, cycle);
  assert.equal(cycle.name, 'cycle');

  assert.equal(vm.call('main', 'dvar').toJS(), 3);
  assert.equal(vm.setDvar('local_dvar', 9), true);
  assert.equal(vm.call('main', 'local').toJS(), 9);

  const thread = vm.start('main', 'worker');
  assert.equal(thread.status(), 'ready');
  vm.runUntilIdle();
  assert.equal(thread.status(), 'waiting_time');
  vm.tick(1);
  vm.runUntilComplete();
  assert.equal(thread.status(), 'finished');
  assert.equal(thread.result().toJS(), 5);

  assert.equal(vm.objectGetField('level', 'name').toJS(), 'test_level');
  assert.equal(vm.objectSetField('level', 'extra', [1, 2]), true);
  assert.deepEqual(vm.objectGetField('level', 'extra').toJS(), [1, 2]);

  assert.equal(vm.getGlobal('missing_global').type, 'undefined');
  assert.equal(vm.setGlobal('test_global', { a: 1 }), true);
  assert.deepEqual(vm.getGlobal('test_global').toJS(), { a: 1 });

  assert.ok(vm.modules().some((module) => module.name === 'main.gsc'));
  assert.ok(vm.moduleBytes('main').length > 0);
  assert.ok(vm.disassembly('main').includes('main'));
  assert.ok(vm.memoryStats().modules >= 1);

  vm.setDebugEnabled(true);
  vm.setBreakpoint('main', 'worker', 0);
  const paused = vm.start('main', 'worker');
  vm.runUntilIdle();
  assert.equal(vm.debugState().paused, true);
  vm.resume();
  vm.runUntilComplete();
  assert.equal(paused.status(), 'finished');
  vm.clearBreakpoints();
} finally {
  vm.close();
}

console.log('gscvm SDK test passed');
