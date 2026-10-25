const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;

const globalState = globalThis;

const nativeCallbacks = globalState.__gscvmNativeCallbacks ??= new Map();
const hosts = globalState.__gscvmHosts ??= new Map();
const hostObjects = globalState.__gscvmHostObjects ??= new Map();
const hostObjectIds = globalState.__gscvmHostObjectIds ??= new WeakMap();
const hostObjectClasses = globalState.__gscvmHostObjectClasses ??= new Map();

function nextId() {
  const id = (globalState.__gscvmNextId ?? 1);
  globalState.__gscvmNextId = id + 1;
  return id;
}

function errorMessage(error) {
  if (error instanceof Error) return error.message;
  return String(error);
}

function isTagged(value) {
  return !!value && typeof value === 'object' && typeof value.$gsc === 'string';
}

function isPlainObject(value) {
  if (!value || typeof value !== 'object') return false;
  if (Array.isArray(value) || value instanceof GscValue) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function keyToTagged(key) {
  if (typeof key === 'number') return key;
  if (/^-?\d+$/.test(key)) {
    const number = Number(key);
    if (Number.isSafeInteger(number) && number >= INT32_MIN && number <= INT32_MAX) return number;
  }
  return key;
}

function encodeNumber(value) {
  if (!Number.isFinite(value)) {
    return { $gsc: 'float', value: Number.isNaN(value) ? 'nan' : value < 0 ? '-inf' : 'inf' };
  }
  if (Number.isInteger(value) && value >= INT32_MIN && value <= INT32_MAX) return value;
  return { $gsc: 'float', value };
}

function encodeEntries(entries, seen) {
  return entries.map(([key, value]) => [keyToTagged(String(key)), encodeValueInner(value, seen)]);
}

export function encodeValue(value) {
  return encodeValueInner(value, new Map());
}

function encodeValueReference(value, seen) {
  const existing = seen.get(value);
  if (existing !== undefined) return { $gsc: 'ref', id: existing };
  const id = seen.size + 1;
  seen.set(value, id);
  return { id };
}

function encodeValueInner(value, seen) {
  if (value instanceof GscValue) return value.raw;
  if (value === undefined || value === null) return null;

  switch (typeof value) {
    case 'boolean':
    case 'string':
      return value;
    case 'number':
      return encodeNumber(value);
    case 'bigint':
      if (value < BigInt(INT32_MIN) || value > BigInt(INT32_MAX)) throw new Error('GSC integers must fit in 32 bits');
      return Number(value);
    case 'function':
      throw new Error('GSC values cannot contain JavaScript functions');
  }

  if (Array.isArray(value)) {
    const reference = encodeValueReference(value, seen);
    if (reference.$gsc) return reference;
    return {
      $gsc: 'array',
      id: reference.id,
      entries: Object.keys(value).map((key) => [keyToTagged(key), encodeValueInner(value[key], seen)]),
    };
  }

  if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) {
    const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    return { $gsc: 'array', entries: Array.from(bytes, (byte, index) => [index, byte]) };
  }

  if (isTagged(value)) {
    switch (value.$gsc) {
      case 'undefined':
        return null;
      case 'integer':
        return Number(value.value);
      case 'float':
        return { $gsc: 'float', value: value.value };
      case 'vector':
        return { $gsc: 'vector', x: Number(value.x), y: Number(value.y), z: Number(value.z) };
      case 'istring':
      case 'hash':
        return { $gsc: value.$gsc, value: String(value.value) };
      case 'function':
        return {
          $gsc: 'function',
          kind: value.kind ?? 'unresolved',
          module: value.module ?? '',
          space: value.space ?? '',
          name: value.name ?? '',
          params: Number(value.params ?? 0),
          flags: Number(value.flags ?? 0),
        };
      case 'thread':
        return { $gsc: 'thread', id: Number(value.id) };
      case 'animtree':
        return { $gsc: 'animtree', name: String(value.name) };
      case 'animation':
        return { $gsc: 'animation', tree: String(value.tree ?? ''), name: String(value.name ?? '') };
      case 'array': {
        const reference = encodeValueReference(value, seen);
        if (reference.$gsc) return reference;
        return {
          $gsc: 'array',
          id: reference.id,
          entries: (value.entries ?? []).map(([key, entry]) => [keyToTagged(String(key)), encodeValueInner(entry, seen)]),
        };
      }
      case 'object': {
        const reference = encodeValueReference(value, seen);
        if (reference.$gsc) return reference;
        return {
          $gsc: 'object',
          id: reference.id,
          class: String(value.class ?? 'object'),
          entries: (value.entries ?? []).map(([key, entry]) => [keyToTagged(String(key)), encodeValueInner(entry, seen)]),
        };
      }
      case 'ref':
        throw new Error('GSC value references cannot be created by the JavaScript host');
      case 'hostobject':
        throw new Error('host objects can only be returned by host callbacks');
      default:
        throw new Error(`unknown GSC value tag '${value.$gsc}'`);
    }
  }

  if (isPlainObject(value)) {
    const reference = encodeValueReference(value, seen);
    if (reference.$gsc) return reference;
    return {
      $gsc: 'object',
      id: reference.id,
      class: String(value.__gscClass ?? 'object'),
      entries: encodeEntries(Object.entries(value).filter(([key]) => key !== '__gscClass'), seen),
    };
  }

  throw new Error('unsupported JavaScript value for GSC');
}

function decodeEntries(entries, target, references) {
  for (const entry of entries ?? []) {
    if (!Array.isArray(entry) || entry.length !== 2) throw new Error('invalid GSC entry');
    const key = decodeValue(entry[0], references);
    if (key === '__gscClass') continue;
    const value = decodeValue(entry[1], references);
    if (Array.isArray(target) && typeof key === 'number') target[key] = value;
    else target[String(key)] = value;
  }
}

export function decodeValue(raw, references = new Map()) {
  if (raw === null || raw === undefined) return undefined;
  if (typeof raw === 'boolean' || typeof raw === 'string' || typeof raw === 'number') return raw;

  if (Array.isArray(raw)) return raw.map((entry) => decodeValue(entry, references));

  if (!isTagged(raw)) {
    const out = {};
    for (const [key, value] of Object.entries(raw)) out[key] = decodeValue(value, references);
    return out;
  }

  switch (raw.$gsc) {
    case 'undefined':
      return undefined;
    case 'ref': {
      if (!references.has(raw.id)) throw new Error(`unknown GSC value reference ${raw.id}`);
      return references.get(raw.id);
    }
    case 'integer':
      return Number(raw.value);
    case 'float':
      if (raw.value === 'nan') return Number.NaN;
      if (raw.value === 'inf') return Number.POSITIVE_INFINITY;
      if (raw.value === '-inf') return Number.NEGATIVE_INFINITY;
      return Number(raw.value);
    case 'istring':
    case 'hash':
      return { $gsc: raw.$gsc, value: raw.value };
    case 'vector':
      return { $gsc: 'vector', x: raw.x, y: raw.y, z: raw.z };
    case 'array': {
      const out = [];
      if (raw.id !== undefined) references.set(raw.id, out);
      decodeEntries(raw.entries, out, references);
      return out;
    }
    case 'object': {
      const out = {};
      Object.defineProperty(out, '__gscClass', { value: raw.class ?? 'object', enumerable: false, configurable: true });
      if (raw.id !== undefined) references.set(raw.id, out);
      decodeEntries(raw.entries, out, references);
      return out;
    }
    case 'function':
      return {
        $gsc: 'function',
        kind: raw.kind ?? 'unresolved',
        module: raw.module ?? '',
        space: raw.space ?? '',
        name: raw.name ?? '',
        params: raw.params ?? 0,
        flags: raw.flags ?? 0,
      };
    case 'thread':
      return { $gsc: 'thread', id: raw.id };
    case 'animtree':
      return { $gsc: 'animtree', name: raw.name };
    case 'animation':
      return { $gsc: 'animation', tree: raw.tree, name: raw.name };
    case 'hostobject':
      return { $gsc: 'hostobject', id: raw.id, class: raw.class };
    default:
      throw new Error(`unknown GSC value tag '${raw.$gsc}'`);
  }
}

function rawType(raw) {
  if (raw === null || raw === undefined) return 'undefined';
  if (typeof raw === 'boolean') return 'boolean';
  if (typeof raw === 'string') return 'string';
  if (typeof raw === 'number') return Number.isInteger(raw) ? 'integer' : 'floating';
  if (typeof raw === 'object' && typeof raw.$gsc === 'string') {
    switch (raw.$gsc) {
      case 'float':
        return 'floating';
      case 'object':
        return 'object';
      case 'array':
        return 'array';
      case 'function':
        return 'function';
      default:
        return raw.$gsc;
    }
  }
  return 'undefined';
}

export class GscValue {
  constructor(type, display, raw) {
    this.type = type;
    this.display = display;
    this.raw = raw;
  }

  static fromJSON(value) {
    return new GscValue(value?.type ?? 'undefined', value?.display ?? '', value?.value ?? null);
  }

  static fromJS(value) {
    const raw = encodeValue(value);
    const decoded = decodeValue(raw);
    return new GscValue(rawType(raw), String(value), raw).withDecoded(decoded);
  }

  static undefined() {
    return new GscValue('undefined', 'undefined', null);
  }

  static integer(value) {
    return new GscValue('integer', String(value), encodeNumber(value));
  }

  static float(value) {
    const raw = Number.isNaN(value) ? 'nan' : value === Number.POSITIVE_INFINITY ? 'inf' : value === Number.NEGATIVE_INFINITY ? '-inf' : value;
    return new GscValue('floating', String(value), { $gsc: 'float', value: raw });
  }

  static string(value) {
    return new GscValue('string', String(value), String(value));
  }

  static vector(x, y, z) {
    return new GscValue('vector', `(${x}, ${y}, ${z})`, { $gsc: 'vector', x, y, z });
  }

  withDecoded(value) {
    this.decoded = value;
    return this;
  }

  toJS() {
    return this.decoded !== undefined ? this.decoded : decodeValue(this.raw);
  }

  valueOf() {
    const value = this.toJS();
    return value !== null && typeof value === 'object' ? this.display : value;
  }

  toString() {
    return this.display;
  }
}

export class NativeContext {
  constructor(vm, meta, rawArgs, args) {
    this.vm = vm;
    this.space = meta.space ?? '';
    this.name = meta.name ?? '';
    this.threadId = meta.threadId;
    this.self = decodeValue(meta.self);
    this.rawArgs = rawArgs;
    this.args = args;
  }

  arg(index) {
    return this.args[index];
  }

  get argumentCount() {
    return this.args.length;
  }

  error(message) {
    throw new Error(String(message));
  }
}

export class GscThread {
  constructor(vm, id) {
    this.vm = vm;
    this.id = id;
  }

  status() {
    return this.vm.threadStatus(this.id);
  }

  result() {
    return this.vm.threadResult(this.id);
  }

  info() {
    return this.vm.threadInfo(this.id);
  }
}

function registerHostObject(object, className) {
  if (object === null || object === undefined) return null;
  if (object instanceof GscValue) return object.raw;
  if (isTagged(object)) return object.$gsc === 'hostobject' ? object : encodeValue(object);
  if (typeof object !== 'object' && typeof object !== 'function') return encodeValue(object);

  let id = hostObjectIds.get(object);
  if (id === undefined) {
    id = nextId();
    hostObjectIds.set(object, id);
    hostObjects.set(id, object);
    hostObjectClasses.set(id, String(className ?? object.__gscClass ?? 'object'));
  }
  return { $gsc: 'hostobject', id, class: hostObjectClasses.get(id) };
}

function hostProperty(object, key) {
  if (object instanceof Map) return object.get(key);
  if (object instanceof Set) return key;
  return object[key];
}

function hostSet(object, key, value) {
  if (object instanceof Map) object.set(key, value);
  else object[key] = value;
}

function hostDelete(object, key) {
  if (object instanceof Map) return object.delete(key);
  if (object instanceof Set) return object.delete(key);
  return delete object[key];
}

function hostHas(object, key) {
  if (object instanceof Map || object instanceof Set) return object.has(key);
  return key in Object(object);
}

function hostKeys(object) {
  if (object instanceof Map) return Array.from(object.keys());
  if (object instanceof Set) return Array.from(object.values());
  return Object.keys(Object(object));
}

function hostSize(object) {
  if (typeof object.size === 'number') return object.size;
  return hostKeys(object).length;
}

function hostObjectResult(id, operation, payload) {
  const object = hostObjects.get(id);
  if (object === undefined) return { $gscError: `unknown host object ${id}` };

  switch (operation) {
    case 'get':
      return encodeValue(hostProperty(object, payload.name));
    case 'set':
      hostSet(object, payload.name, decodeValue(payload.value));
      return true;
    case 'clear':
      return hostDelete(object, payload.name);
    case 'has':
      return hostHas(object, payload.name);
    case 'keys':
      return hostKeys(object).map((key) => encodeValue(key));
    case 'size':
      return hostSize(object);
    case 'getIndex':
      return encodeValue(hostProperty(object, decodeValue(payload.key)));
    case 'setIndex':
      hostSet(object, decodeValue(payload.key), decodeValue(payload.value));
      return true;
    case 'clearIndex':
      return hostDelete(object, decodeValue(payload.key));
    case 'toString':
      return String(object);
    default:
      return { $gscError: `unsupported host object operation '${operation}'` };
  }
}

function hostResult(host, operation, payload) {
  switch (operation) {
    case 'log':
      (host.log ?? host.onLog)?.(payload.level, payload.message);
      return null;
    case 'dvar':
      return encodeValue(host.dvar ? host.dvar(payload.name) : undefined);
    case 'dvarInt': {
      const value = host.dvarInt ? host.dvarInt(payload.name) : host.dvar?.(payload.name);
      return value === undefined || value === null ? null : Number(value);
    }
    case 'dvarFloat': {
      const value = host.dvarFloat ? host.dvarFloat(payload.name) : host.dvar?.(payload.name);
      return value === undefined || value === null ? null : Number(value);
    }
    case 'dvarVector': {
      const value = host.dvarVector ? host.dvarVector(payload.name) : host.dvar?.(payload.name);
      if (value === undefined || value === null) return null;
      if (Array.isArray(value)) return value.map(Number);
      return [Number(value.x), Number(value.y), Number(value.z)];
    }
    case 'loadScript': {
      const value = host.loadScript?.(payload.name);
      if (value === undefined || value === null) return null;
      if (value instanceof ArrayBuffer) return Array.from(new Uint8Array(value));
      if (ArrayBuffer.isView(value)) return Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
      if (Array.isArray(value)) return value.map((byte) => Number(byte) & 255);
      if (typeof value === 'string') return value;
      throw new Error('loadScript must return a string, byte array, ArrayBuffer, or null');
    }
    case 'level':
    case 'game':
    case 'world':
    case 'classes':
    case 'anim': {
      const method = host[operation] ?? host[`${operation}Object`];
      const value = typeof method === 'function' ? method.call(host) : (host[operation] ?? host.objects?.[operation]);
      if (value === undefined || value === null) return null;
      return registerHostObject(value, operation);
    }
    case 'createObject': {
      const method = host.createObject ?? host.create_object;
      if (!method) return null;
      const value = method.call(host, payload.className);
      if (value === undefined || value === null) return null;
      return registerHostObject(value, payload.className);
    }
    default:
      return { $gscError: `unsupported host operation '${operation}'` };
  }
}

function installBridges() {
  if (!globalState.__gscvmNativeCall) {
    globalState.__gscvmNativeCall = (id, metaJson, argsJson) => {
      const entry = nativeCallbacks.get(id);
      if (!entry) return JSON.stringify({ $gscError: `unknown native callback ${id}` });
      try {
        const meta = JSON.parse(metaJson);
        const rawArgs = JSON.parse(argsJson);
        const references = new Map();
        const args = rawArgs.map((arg) => decodeValue(arg, references));
        const context = new NativeContext(entry.vm, meta, rawArgs, args);
        let result = entry.fn(context, ...args);
        if (result && typeof result.then === 'function') {
          throw new Error('native callbacks must be synchronous; async functions are not supported');
        }
        return JSON.stringify(encodeValue(result));
      } catch (error) {
        return JSON.stringify({ $gscError: errorMessage(error) });
      }
    };
  }

  if (!globalState.__gscvmHostCall) {
    globalState.__gscvmHostCall = (id, operation, payloadJson) => {
      const host = hosts.get(id);
      if (!host) return JSON.stringify({ $gscError: `unknown JavaScript host ${id}` });
      try {
        const payload = payloadJson ? JSON.parse(payloadJson) : {};
        return JSON.stringify(hostResult(host, operation, payload));
      } catch (error) {
        return JSON.stringify({ $gscError: errorMessage(error) });
      }
    };
  }

  if (!globalState.__gscvmHostObjectCall) {
    globalState.__gscvmHostObjectCall = (id, operation, payloadJson) => {
      try {
        const payload = payloadJson ? JSON.parse(payloadJson) : {};
        return JSON.stringify(hostObjectResult(id, operation, payload));
      } catch (error) {
        return JSON.stringify({ $gscError: errorMessage(error) });
      }
    };
  }
}

installBridges();

export class GscVM {
  constructor(Module, options = {}) {
    this.Module = Module;
    this.options = options;
    this.host = options.host ?? null;
    this.hostId = this.host ? this._registerHost(this.host) : 0;
    this.nativeIds = new Set();
    this.closed = false;
  }

  static async create(options = {}) {
    const factory = options.factory ?? globalThis.createGscvmModule;
    if (!options.module && typeof factory !== 'function') {
      throw new Error('createGscvmModule is not available; pass options.factory or load gscvm.js first');
    }
    const Module = options.module ?? await factory(options.moduleOptions ?? {});
    const vm = new GscVM(Module, options);
    vm.init(options.config ?? {});
    return vm;
  }

  _registerHost(host) {
    const id = nextId();
    hosts.set(id, host);
    return id;
  }

  _check() {
    if (this.closed) throw new Error('gscvm instance is closed');
  }

  _callString(name, types = [], args = []) {
    const pointer = this.Module.ccall(name, 'number', types, args);
    if (!pointer) return null;
    try {
      return this.Module.UTF8ToString(pointer);
    } finally {
      this.Module._gscvm_free(pointer);
    }
  }

  _callNumber(name, types = [], args = []) {
    return this.Module.ccall(name, 'number', types, args);
  }

  _json(name, types = [], args = [], allowNull = false) {
    const text = this._callString(name, types, args);
    if (text === null) {
      if (allowNull) return null;
      throw new Error(this.lastError());
    }
    return JSON.parse(text);
  }

  _ok(name, types = [], args = []) {
    const result = this._json(name, types, args);
    if (!result || result.ok === false) throw new Error(result?.error ?? this.lastError());
    return result;
  }

  _value(name, types = [], args = []) {
    const result = this._json(name, types, args);
    if (result && result.ok === false) throw new Error(result.error ?? this.lastError());
    return GscValue.fromJSON(result);
  }

  _bool(name, types = [], args = []) {
    return this._callNumber(name, types, args) !== 0;
  }

  lastError() {
    const pointer = this.Module._gscvm_last_error();
    return pointer ? this.Module.UTF8ToString(pointer) : 'unknown gscvm error';
  }

  init(config = {}) {
    this._check();
    this._callNumber('gscvm_set_js_host', ['number'], [this.hostId]);
    const ok = this._callNumber('gscvm_init_ex', ['string'], [JSON.stringify(config ?? {})]);
    if (!ok) throw new Error(this.lastError());
    return this;
  }

  close() {
    if (this.closed) return;
    for (const id of this.nativeIds) nativeCallbacks.delete(id);
    this.nativeIds.clear();
    if (this.hostId) hosts.delete(this.hostId);
    this.Module._gscvm_shutdown();
    this.closed = true;
  }

  shutdown() {
    this.close();
  }

  config() {
    return this._json('gscvm_config');
  }

  setInstance(client) {
    return this._bool('gscvm_set_instance', ['number'], [client ? 1 : 0]);
  }

  setFrameTime(seconds) {
    return this._bool('gscvm_set_frame_time', ['number'], [seconds]);
  }

  setRealTime(enabled) {
    return this._bool('gscvm_set_real_time', ['number'], [enabled ? 1 : 0]);
  }

  setTickRate(rate) {
    return this._bool('gscvm_set_tick_rate', ['number'], [rate]);
  }

  setExecutionMode(mode) {
    const value = typeof mode === 'string' ? (mode === 'tick_based' || mode === 'tick' ? 1 : 0) : (mode ? 1 : 0);
    return this._bool('gscvm_set_execution_mode', ['number'], [value]);
  }

  setInstructionLimit(limit) {
    return this._bool('gscvm_set_instruction_limit', ['number'], [limit]);
  }

  setMaxCallDepth(depth) {
    return this._bool('gscvm_set_max_call_depth', ['number'], [depth]);
  }

  setMaxStackSize(size) {
    return this._bool('gscvm_set_max_stack_size', ['number'], [size]);
  }

  setMaxThreads(count) {
    return this._bool('gscvm_set_max_threads', ['number'], [count]);
  }

  setExecuteDevblocks(enabled) {
    return this._bool('gscvm_set_execute_devblocks', ['number'], [enabled ? 1 : 0]);
  }

  setT6Fixup(enabled) {
    return this._bool('gscvm_set_t6_fixup', ['number'], [enabled ? 1 : 0]);
  }

  setDebugEnabled(enabled) {
    return this._bool('gscvm_set_debug_enabled', ['number'], [enabled ? 1 : 0]);
  }

  addSearchPath(path) {
    return this._bool('gscvm_add_search_path', ['string'], [path]);
  }

  clearSearchPaths() {
    this._callNumber('gscvm_clear_search_paths');
  }

  setSearchPaths(paths) {
    this.clearSearchPaths();
    for (const path of paths) this.addSearchPath(path);
  }

  searchPaths() {
    return this._json('gscvm_search_paths');
  }

  addSource(path, source) {
    return this._bool('gscvm_add_source', ['string', 'string'], [path, source]);
  }

  addFile(path, data) {
    const bytes = typeof data === 'string'
      ? new TextEncoder().encode(data)
      : data instanceof ArrayBuffer
        ? new Uint8Array(data)
        : ArrayBuffer.isView(data)
          ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
          : new Uint8Array(data);
    const pointer = this.Module._malloc(bytes.length);
    if (!pointer && bytes.length) throw new Error('could not allocate WebAssembly memory');
    try {
      this.Module.HEAPU8.set(bytes, pointer);
      return this._bool('gscvm_add_file', ['string', 'number', 'number'], [path, pointer, bytes.length]);
    } finally {
      this.Module._free(pointer);
    }
  }

  removeFile(path) {
    return this._bool('gscvm_remove_file', ['string'], [path]);
  }

  clearFiles() {
    this._callNumber('gscvm_clear_files');
  }

  listFiles() {
    return this._json('gscvm_list_files');
  }

  readFile(path) {
    const result = this._ok('gscvm_read_file', ['string'], [path]);
    return new Uint8Array(result.data ?? []);
  }

  resolvePath(path) {
    return this._callString('gscvm_resolve_path', ['string'], [path]);
  }

  loadFile(path, name = '') {
    return this._ok('gscvm_load_file', ['string', 'string'], [path, name]).module;
  }

  loadSource(name, source) {
    return this._ok('gscvm_load_source', ['string', 'string'], [name, source]).module;
  }

  loadModule(name, data) {
    const bytes = typeof data === 'string'
      ? new TextEncoder().encode(data)
      : data instanceof ArrayBuffer
        ? new Uint8Array(data)
        : ArrayBuffer.isView(data)
          ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
          : new Uint8Array(data);
    const pointer = this.Module._malloc(bytes.length);
    if (!pointer && bytes.length) throw new Error('could not allocate WebAssembly memory');
    try {
      this.Module.HEAPU8.set(bytes, pointer);
      return this._ok('gscvm_load_module', ['string', 'number', 'number'], [name, pointer, bytes.length]).module;
    } finally {
      this.Module._free(pointer);
    }
  }

  loadDependencies(module) {
    this._ok('gscvm_load_dependencies', ['string'], [module]);
  }

  hasModule(module) {
    return this._bool('gscvm_has_module', ['string'], [module]);
  }

  moduleInfo(module) {
    return this._json('gscvm_module_info', ['string'], [module]);
  }

  modules() {
    return this._json('gscvm_modules');
  }

  disassembly(module) {
    return this._callString('gscvm_disassembly', ['string'], [module]);
  }

  moduleBytes(module) {
    const text = this._callString('gscvm_module_bytes', ['string'], [module]);
    if (text === null) throw new Error(this.lastError());
    const bytes = new Uint8Array(text.length / 2);
    for (let index = 0; index < bytes.length; index += 1) {
      bytes[index] = Number.parseInt(text.slice(index * 2, index * 2 + 2), 16);
    }
    return bytes;
  }

  call(module, functionName, ...args) {
    const result = this._ok('gscvm_call_values', ['string', 'string', 'string'], [module ?? '', functionName, JSON.stringify(args.map(encodeValue))]);
    return GscValue.fromJSON(result.result);
  }

  callFunction(functionValue, ...args) {
    const raw = functionValue instanceof GscValue ? functionValue.raw : encodeValue(functionValue);
    const result = this._ok('gscvm_call_function', ['string', 'string'], [JSON.stringify(raw), JSON.stringify(args.map(encodeValue))]);
    return GscValue.fromJSON(result.result);
  }

  start(module, functionName, ...args) {
    const result = this._ok('gscvm_start_values', ['string', 'string', 'string'], [module ?? '', functionName, JSON.stringify(args.map(encodeValue))]);
    return new GscThread(this, result.thread);
  }

  startFunction(functionValue, ...args) {
    const raw = functionValue instanceof GscValue ? functionValue.raw : encodeValue(functionValue);
    const result = this._ok('gscvm_start_function', ['string', 'string'], [JSON.stringify(raw), JSON.stringify(args.map(encodeValue))]);
    return new GscThread(this, result.thread);
  }

  threadStatus(id) {
    return this._json('gscvm_thread_status', ['number'], [id]).status;
  }

  threadResult(id) {
    return this._value('gscvm_thread_result_values', ['number'], [id]);
  }

  threadInfo(id) {
    return this._json('gscvm_thread_info', ['number'], [id]);
  }

  threads() {
    return this._json('gscvm_threads');
  }

  tick(delta) {
    return this._ok('gscvm_tick', ['number'], [delta]);
  }

  runUntilIdle() {
    return this._ok('gscvm_run_until_idle');
  }

  runUntilComplete() {
    return this._ok('gscvm_run_until_complete');
  }

  notify(event, ...args) {
    return this._ok('gscvm_notify_values', ['string', 'string', 'string'], ['', event, JSON.stringify(args.map(encodeValue))]).resumed;
  }

  notifyTarget(target, event, ...args) {
    const raw = target instanceof GscValue ? target.raw : encodeValue(target);
    return this._ok('gscvm_notify_values', ['string', 'string', 'string'], [JSON.stringify(raw), event, JSON.stringify(args.map(encodeValue))]).resumed;
  }

  time() {
    return this._callNumber('gscvm_time');
  }

  instructionCount() {
    return this._callNumber('gscvm_instruction_count');
  }

  hasRunnableThreads() {
    return this._bool('gscvm_has_runnable_threads');
  }

  hasLiveThreads() {
    return this._bool('gscvm_has_live_threads');
  }

  globals() {
    return this._json('gscvm_globals');
  }

  inspect(value, depth = 4, items = 256) {
    const raw = value instanceof GscValue ? value.raw : encodeValue(value);
    return this._json('gscvm_inspect_value', ['string', 'number', 'number'], [JSON.stringify(raw), depth, items]);
  }

  getGlobal(name) {
    return this._value('gscvm_get_global', ['string'], [name]);
  }

  setGlobal(name, value) {
    const raw = value instanceof GscValue ? value.raw : encodeValue(value);
    return this._bool('gscvm_set_global', ['string', 'string'], [name, JSON.stringify(raw)]);
  }

  object(kind) {
    return this._value('gscvm_object', ['string'], [kind]);
  }

  objectGetField(kind, name) {
    return this._value('gscvm_object_get_field', ['string', 'string'], [kind, name]);
  }

  objectSetField(kind, name, value) {
    const raw = value instanceof GscValue ? value.raw : encodeValue(value);
    return this._bool('gscvm_object_set_field', ['string', 'string', 'string'], [kind, name, JSON.stringify(raw)]);
  }

  objectClearField(kind, name) {
    return this._bool('gscvm_object_clear_field', ['string', 'string'], [kind, name]);
  }

  objectHasField(kind, name) {
    return this._bool('gscvm_object_has_field', ['string', 'string'], [kind, name]);
  }

  objectKeys(kind) {
    const result = this._ok('gscvm_object_keys', ['string'], [kind]);
    const references = new Map();
    return (result.keys ?? []).map((key) => decodeValue(key, references));
  }

  objectGetIndex(kind, key) {
    const raw = key instanceof GscValue ? key.raw : encodeValue(key);
    return this._value('gscvm_object_get_index', ['string', 'string'], [kind, JSON.stringify(raw)]);
  }

  objectSetIndex(kind, key, value) {
    const rawKey = key instanceof GscValue ? key.raw : encodeValue(key);
    const rawValue = value instanceof GscValue ? value.raw : encodeValue(value);
    return this._bool('gscvm_object_set_index', ['string', 'string', 'string'], [kind, JSON.stringify(rawKey), JSON.stringify(rawValue)]);
  }

  objectClearIndex(kind, key) {
    const raw = key instanceof GscValue ? key.raw : encodeValue(key);
    return this._bool('gscvm_object_clear_index', ['string', 'string'], [kind, JSON.stringify(raw)]);
  }

  objectSize(kind) {
    return this._callNumber('gscvm_object_size', ['string'], [kind]);
  }

  registerNative(space, name, callback) {
    if (typeof name === 'function') {
      callback = name;
      name = space;
      space = '';
    }
    if (typeof callback !== 'function') throw new Error('native callback must be a function');
    const separator = String(name).indexOf('::');
    if (separator !== -1) {
      space = String(name).slice(0, separator);
      name = String(name).slice(separator + 2);
    }
    const id = nextId();
    nativeCallbacks.set(id, { fn: callback, vm: this });
    const ok = this._bool('gscvm_register_js_native', ['string', 'string', 'number'], [space ?? '', name, id]);
    if (!ok) {
      nativeCallbacks.delete(id);
      throw new Error(this.lastError());
    }
    this.nativeIds.add(id);
    return () => this.unregisterNative(space ?? '', name);
  }

  unregisterNative(space, name) {
    if (typeof name === 'undefined') {
      name = space;
      space = '';
    }
    return this._bool('gscvm_unregister_native', ['string', 'string'], [space ?? '', name]);
  }

  hasNative(space, name) {
    if (typeof name === 'undefined') {
      name = space;
      space = '';
    }
    return this._bool('gscvm_has_native', ['string', 'string'], [space ?? '', name]);
  }

  natives() {
    return this._json('gscvm_natives');
  }

  setDvar(name, value) {
    const raw = value instanceof GscValue ? value.raw : encodeValue(value);
    return this._bool('gscvm_set_dvar', ['string', 'string'], [name, JSON.stringify(raw)]);
  }

  memoryStats() {
    return this._json('gscvm_memory_stats');
  }

  memoryBlocks(previewBytes = 64) {
    return this._json('gscvm_memory_blocks', ['number'], [previewBytes]);
  }

  setBreakpoint(module, functionName, pc) {
    return this._ok('gscvm_set_breakpoint', ['string', 'string', 'number'], [module ?? '', functionName, pc]);
  }

  clearBreakpoints() {
    this._ok('gscvm_clear_breakpoints');
  }

  breakpoints() {
    return this._json('gscvm_breakpoints');
  }

  debugState() {
    return this._json('gscvm_debug_state');
  }

  step() {
    return this._ok('gscvm_step');
  }

  resume() {
    return this._ok('gscvm_resume');
  }

  pause() {
    this._ok('gscvm_pause');
  }

  takeOutput() {
    const text = this._callString('gscvm_take_output');
    return text ?? '';
  }
}

export async function createVM(options = {}) {
  return GscVM.create(options);
}

export default createVM;
