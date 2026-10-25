export type GscKey = string | number | GscValue;
export type GscObjectKind = 'level' | 'game' | 'world' | 'classes' | 'anim';
export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error' | 'none' | string;

export interface Host {
  log?: (level: LogLevel, message: string) => void;
  onLog?: (level: LogLevel, message: string) => void;
  dvar?: (name: string) => unknown;
  dvarInt?: (name: string) => number | null | undefined;
  dvarFloat?: (name: string) => number | null | undefined;
  dvarVector?: (name: string) => ArrayLike<number> | { x: number; y: number; z: number } | null | undefined;
  loadScript?: (name: string) => string | ArrayLike<number> | ArrayBuffer | null | undefined;
  level?: () => unknown;
  levelObject?: () => unknown;
  game?: () => unknown;
  gameObject?: () => unknown;
  world?: () => unknown;
  worldObject?: () => unknown;
  classes?: () => unknown;
  classesObject?: () => unknown;
  anim?: () => unknown;
  animObject?: () => unknown;
  createObject?: (className: string) => unknown;
  create_object?: (className: string) => unknown;
  objects?: Partial<Record<GscObjectKind, unknown>>;
}

export interface VMConfig {
  instance?: 'server' | 'client' | 0 | 1;
  executionMode?: 'real_time' | 'real-time' | 'tick_based' | 'tick' | 0 | 1;
  tickRate?: number;
  frameTime?: number;
  realTime?: boolean;
  instructionLimit?: number;
  maxCallDepth?: number;
  maxStackSize?: number;
  maxThreads?: number;
  executeDevblocks?: boolean;
  t6Fixup?: boolean;
  searchPaths?: string[];
}

export interface GscvmModule {
  ccall(name: string, returnType: string, argumentTypes: string[], args: unknown[]): number;
  UTF8ToString(pointer: number): string;
  stringToUTF8(value: string, pointer: number, maxBytes: number): void;
  lengthBytesUTF8(value: string): number;
  HEAPU8: Uint8Array;
  _malloc(size: number): number;
  _free(pointer: number): void;
  _gscvm_init(): number;
  _gscvm_init_ex(config: string | null): number;
  _gscvm_shutdown(): void;
  _gscvm_last_error(): number;
  _gscvm_free(pointer: number): void;
  [key: string]: unknown;
}

export type GscvmFactory = (options?: Record<string, unknown>) => Promise<GscvmModule>;

export interface CreateVMOptions {
  factory?: GscvmFactory;
  module?: GscvmModule;
  moduleOptions?: Record<string, unknown>;
  host?: Host;
  config?: VMConfig;
}

export interface VMConfigInfo {
  instance: 'server' | 'client';
  executionMode: 'real_time' | 'tick_based';
  tickRate: number;
  frameTime: number;
  realTime: boolean;
  instructionLimit: number;
  maxCallDepth: number;
  maxStackSize: number;
  maxThreads: number;
  executeDevblocks: boolean;
  t6Fixup: boolean;
  hostId: number;
  searchPaths: string[];
}

export interface ThreadInfo {
  id: number;
  status: string;
  wakeTime: number;
  waitEvent: string;
  waitTarget: string;
  result: string;
  error: string;
  endonCount: number;
  frames: unknown[];
}

export interface ModuleInfo {
  name: string;
  includes: string[];
  functions: Array<{
    name: string;
    space: string;
    params: number;
    flags: number;
    offset: number;
    size: number;
  }>;
}

export interface BreakpointInfo {
  module: string;
  function: string;
  pc: number;
}

export interface DebugState {
  enabled: boolean;
  paused: boolean;
  thread: number;
  module: string;
  function: string;
  pc: number;
  offset: number;
  opcode: string;
  breakpoints: BreakpointInfo[];
}

export interface MemoryStats {
  modules: number;
  functions: number;
  instructions: number;
  moduleBytes: number;
  threads: number;
  liveThreads: number;
  frames: number;
  locals: number;
  stackItems: number;
  arrays: number;
  objects: number;
  values: number;
  nestedValues: number;
  estimatedBytes: number;
}

export interface InspectValue {
  name: string;
  type: string;
  value: string;
  expandable: boolean;
  childCount: number;
  children?: InspectValue[];
}

export interface NativeContext {
  vm: GscVM;
  space: string;
  name: string;
  threadId: number;
  self: unknown;
  rawArgs: unknown[];
  args: unknown[];
  readonly argumentCount: number;
  arg(index: number): unknown;
  error(message: string): never;
}

export type NativeCallback = (context: NativeContext, ...args: unknown[]) => unknown;

export class GscValue {
  constructor(type: string, display: string, raw: unknown);
  type: string;
  display: string;
  raw: unknown;
  decoded?: unknown;
  static fromJSON(value: { type?: string; display?: string; value?: unknown } | null | undefined): GscValue;
  static fromJS(value: unknown): GscValue;
  static undefined(): GscValue;
  static integer(value: number): GscValue;
  static float(value: number): GscValue;
  static string(value: string): GscValue;
  static vector(x: number, y: number, z: number): GscValue;
  withDecoded(value: unknown): this;
  toJS(): unknown;
  valueOf(): unknown;
  toString(): string;
}

export class GscThread {
  constructor(vm: GscVM, id: number);
  vm: GscVM;
  id: number;
  status(): string;
  result(): GscValue;
  info(): ThreadInfo;
}

export class GscVM {
  constructor(Module: GscvmModule, options?: CreateVMOptions);
  static create(options?: CreateVMOptions): Promise<GscVM>;
  Module: GscvmModule;
  options: CreateVMOptions;
  host: Host | null;
  hostId: number;
  nativeIds: Set<number>;
  closed: boolean;

  init(config?: VMConfig): this;
  close(): void;
  shutdown(): void;
  config(): VMConfigInfo;
  lastError(): string;

  setInstance(client: boolean): boolean;
  setFrameTime(seconds: number): boolean;
  setRealTime(enabled: boolean): boolean;
  setTickRate(rate: number): boolean;
  setExecutionMode(mode: 'real_time' | 'real-time' | 'tick_based' | 'tick' | 0 | 1 | boolean): boolean;
  setInstructionLimit(limit: number): boolean;
  setMaxCallDepth(depth: number): boolean;
  setMaxStackSize(size: number): boolean;
  setMaxThreads(count: number): boolean;
  setExecuteDevblocks(enabled: boolean): boolean;
  setT6Fixup(enabled: boolean): boolean;
  setDebugEnabled(enabled: boolean): boolean;

  addSearchPath(path: string): boolean;
  clearSearchPaths(): void;
  setSearchPaths(paths: string[]): void;
  searchPaths(): string[];

  addSource(path: string, source: string): boolean;
  addFile(path: string, data: string | ArrayBuffer | ArrayBufferView): boolean;
  removeFile(path: string): boolean;
  clearFiles(): void;
  listFiles(): string[];
  readFile(path: string): Uint8Array;
  resolvePath(path: string): string | null;

  loadFile(path: string, name?: string): ModuleInfo;
  loadSource(name: string, source: string): ModuleInfo;
  loadModule(name: string, data: string | ArrayBuffer | ArrayBufferView): ModuleInfo;
  loadDependencies(module: string): void;
  hasModule(module: string): boolean;
  moduleInfo(module: string): ModuleInfo;
  modules(): ModuleInfo[];
  disassembly(module: string): string | null;
  moduleBytes(module: string): Uint8Array;

  call(module: string, functionName: string, ...args: unknown[]): GscValue;
  callFunction(functionValue: GscValue | unknown, ...args: unknown[]): GscValue;
  start(module: string, functionName: string, ...args: unknown[]): GscThread;
  startFunction(functionValue: GscValue | unknown, ...args: unknown[]): GscThread;

  threadStatus(id: number): string;
  threadResult(id: number): GscValue;
  threadInfo(id: number): ThreadInfo;
  threads(): ThreadInfo[];

  tick(delta?: number): { ok: true; executed: number; time: number; debug: DebugState };
  runUntilIdle(): { ok: true; executed: number; debug: DebugState };
  runUntilComplete(): { ok: true; executed: number; debug: DebugState };
  notify(event: string, ...args: unknown[]): number;
  notifyTarget(target: GscValue | unknown, event: string, ...args: unknown[]): number;
  time(): number;
  instructionCount(): number;
  hasRunnableThreads(): boolean;
  hasLiveThreads(): boolean;

  globals(): unknown;
  inspect(value: GscValue | unknown, depth?: number, items?: number): InspectValue;
  getGlobal(name: string): GscValue;
  setGlobal(name: string, value: GscValue | unknown): boolean;

  object(kind: GscObjectKind): GscValue;
  objectGetField(kind: GscObjectKind, name: string): GscValue;
  objectSetField(kind: GscObjectKind, name: string, value: GscValue | unknown): boolean;
  objectClearField(kind: GscObjectKind, name: string): boolean;
  objectHasField(kind: GscObjectKind, name: string): boolean;
  objectKeys(kind: GscObjectKind): unknown[];
  objectGetIndex(kind: GscObjectKind, key: GscKey): GscValue;
  objectSetIndex(kind: GscObjectKind, key: GscKey, value: GscValue | unknown): boolean;
  objectClearIndex(kind: GscObjectKind, key: GscKey): boolean;
  objectSize(kind: GscObjectKind): number;

  registerNative(space: string, name: string, callback: NativeCallback): () => boolean;
  registerNative(name: string, callback: NativeCallback): () => boolean;
  unregisterNative(space: string, name?: string): boolean;
  hasNative(space: string, name?: string): boolean;
  natives(): string[];

  setDvar(name: string, value: GscValue | unknown): boolean;
  memoryStats(): MemoryStats;
  memoryBlocks(previewBytes?: number): unknown[];

  setBreakpoint(module: string, functionName: string, pc: number): { ok: boolean; debug: DebugState };
  clearBreakpoints(): void;
  breakpoints(): BreakpointInfo[];
  debugState(): DebugState;
  step(): { ok: true; executed: number; debug: DebugState };
  resume(): { ok: true; executed: number; debug: DebugState };
  pause(): void;
  takeOutput(): string;
}

export function encodeValue(value: unknown): unknown;
export function decodeValue(raw: unknown, references?: Map<number, unknown>): unknown;
export function createVM(options?: CreateVMOptions): Promise<GscVM>;
export default createVM;
