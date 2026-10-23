// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gsc/error.hpp"
#include "gsc/t6/host.hpp"
#include "gsc/t6/native.hpp"
#include "gsc/t6/value.hpp"

namespace gsc::t6
{

enum class Instance : std::uint8_t
{
    server,
    client,
};

enum class ExecutionMode : std::uint8_t
{
    // Advance directly to the next wait deadline.
    real_time,
    // Advance in fixed steps of 1 / tick_rate. Waits resume on tick boundaries.
    tick_based,
};

struct VMConfig
{
    Instance instance{ Instance::server };

    // Selects how scheduling advances time. Explicit tick(dt) calls use the
    // host-provided delta in real_time mode and round it up to whole ticks in
    // tick_based mode.
    ExecutionMode execution_mode{ ExecutionMode::real_time };

    // Tick frequency for ExecutionMode::tick_based. Any positive finite value is
    // accepted (for example 20, 40 or 60).
    double tick_rate{ 20.0 };

    // Logical time advanced by one automatic frame wait in real_time mode.
    double frame_time{ 1.0 / 60.0 };

    // Wall-clock pacing for automatic time advancement in call() and
    // run_until_complete(). When false, waits are fast-forwarded as quickly as
    // possible while preserving the selected execution mode's step boundaries.
    bool real_time{ true };

    // Safety limits. Zero disables the corresponding limit. The instruction
    // limit is a per-scheduling-run budget and doubles as infinite-loop
    // detection; the call-depth limit bounds recursive script calls.
    std::size_t instruction_limit{ 10'000'000 };
    std::size_t max_call_depth{ 1024 };
    std::size_t max_stack_size{ 65'536 };
    std::size_t max_threads{ 4'096 };

    // Dev blocks are skipped by default, matching a production build.
    bool execute_devblocks{ false };

    // Reproduce the upstream --t6fixup loader behaviour.
    bool t6_fixup{ false };

    std::vector<std::filesystem::path> search_paths;

    std::shared_ptr<Host> host;
};

struct FunctionInfo
{
    std::string name;
    std::string space;
    std::uint8_t params{};
    std::uint8_t flags{};
    std::size_t offset{};
    std::size_t size{};
};

struct ModuleInfo
{
    std::string name;
    std::vector<std::string> includes;
    std::vector<FunctionInfo> functions;
};

enum class ThreadStatus : std::uint8_t
{
    unknown,
    ready,
    running,
    waiting_time,
    waiting_frame,
    waiting_event,
    finished,
    failed,
    aborted,
    paused,
};

// Structured value inspection used by embedders and the browser playground.
// Containers are expanded recursively up to InspectOptions::max_depth.
struct VariableInfo
{
    std::string name;
    std::string type;
    std::string value;
    bool expandable{};
    std::size_t child_count{};
    std::vector<VariableInfo> children;
};

struct InspectOptions
{
    std::size_t max_depth{ 4 };
    std::size_t max_items{ 256 };
};

struct FrameInfo
{
    std::string module;
    std::string function;
    std::size_t pc{};
    std::size_t offset{};
    std::string opcode;
    std::size_t max_stack{};
    std::vector<VariableInfo> locals;
    std::vector<VariableInfo> varargs;
    std::vector<VariableInfo> stack;
};

struct ThreadInfo
{
    ThreadRef thread;
    ThreadStatus status{ ThreadStatus::unknown };
    double wake_time{};
    std::string wait_event;
    std::string wait_target;
    std::string result;
    std::string error;
    std::size_t endon_count{};
    std::vector<FrameInfo> frames;
};

struct MemoryStats
{
    std::size_t modules{};
    std::size_t functions{};
    std::size_t instructions{};
    std::size_t module_bytes{};
    std::size_t threads{};
    std::size_t live_threads{};
    std::size_t frames{};
    std::size_t locals{};
    std::size_t stack_items{};
    std::size_t arrays{};
    std::size_t objects{};
    std::size_t values{};
    std::size_t nested_values{};
    std::size_t estimated_bytes{};
};

// Logical memory regions. Pointers are intentionally not exposed because they
// are not stable across host builds or WebAssembly linear-memory growth.
struct MemoryBlockInfo
{
    std::string kind;
    std::string name;
    std::size_t size{};
    std::string preview_hex;
};

struct BreakpointInfo
{
    std::string module;
    std::string function;
    std::size_t pc{};

    friend auto operator==(BreakpointInfo const&, BreakpointInfo const&) -> bool = default;
};

struct DebugState
{
    bool enabled{};
    bool paused{};
    ThreadRef thread;
    std::string module;
    std::string function;
    std::string opcode;
    std::size_t pc{};
    std::size_t offset{};
    std::vector<BreakpointInfo> breakpoints;
};

// Detailed runtime diagnostic. The VM keeps this information on the exception
// so that CLI users and embedding applications can identify the failing frame.
class RuntimeError : public Error
{
public:
    RuntimeError(std::string message, std::string module, std::string function, std::size_t offset, std::string opcode, std::uint64_t thread);

    auto module() const -> std::string const& { return module_; }
    auto function() const -> std::string const& { return function_; }
    auto offset() const -> std::size_t { return offset_; }
    auto opcode() const -> std::string const& { return opcode_; }
    auto thread_id() const -> std::uint64_t { return thread_id_; }

private:
    std::string module_;
    std::string function_;
    std::size_t offset_{};
    std::string opcode_;
    std::uint64_t thread_id_{};
};

class VM
{
public:
    explicit VM(VMConfig config = {});
    ~VM();

    VM(VM const&) = delete;
    VM(VM&&) noexcept;
    auto operator=(VM const&) -> VM& = delete;
    auto operator=(VM&&) noexcept -> VM&;

    auto config() const -> VMConfig const&;
    auto natives() -> NativeRegistry&;
    auto natives() const -> NativeRegistry const&;

    auto host() -> Host&;
    auto host() const -> Host const&;
    auto set_host(std::shared_ptr<Host> host) -> void;

    auto add_search_path(std::filesystem::path path) -> void;
    auto clear_search_paths() -> void;
    auto search_paths() const -> std::vector<std::filesystem::path> const&;

    // In-memory script files. These are checked before the host and filesystem
    // loaders, and are also used when resolving #include dependencies.
    // Paths use forward slashes and may be relative to any configured search
    // path. `resolve_virtual_path` returns the canonical in-memory path.
    auto add_virtual_file(std::string path, std::span<std::uint8_t const> data) -> void;
    auto add_virtual_file(std::string path, std::string_view text) -> void;
    auto remove_virtual_file(std::string_view path) -> bool;
    auto clear_virtual_files() -> void;
    auto virtual_files() const -> std::vector<std::string>;
    auto resolve_virtual_path(std::string_view path) const -> std::optional<std::string>;
    auto read_virtual_file(std::string_view path) const -> std::optional<std::vector<std::uint8_t>>;

    // Loading. `name` is the canonical script name used for dependency
    // resolution (for example "common/utilities"). If the compiled header also
    // carries a script name, both spellings are registered as aliases.
    auto load_module(std::string name, std::span<std::uint8_t const> data) -> ModuleInfo;
    auto load_module(std::string name, std::vector<std::uint8_t> data) -> ModuleInfo;
    auto load_module_file(std::filesystem::path const& path, std::optional<std::string> name = std::nullopt) -> ModuleInfo;
    auto load_dependencies(std::string_view module) -> void;

    auto has_module(std::string_view name) const -> bool;
    auto module_info(std::string_view name) const -> ModuleInfo const&;
    auto modules() const -> std::vector<ModuleInfo>;
    auto disassembly(std::string_view name) const -> std::string const&;
    auto module_bytes(std::string_view name) const -> std::vector<std::uint8_t>;

    // Calling. `call` runs the invocation thread and every spawned thread to
    // completion. Timed/frame waits are advanced automatically. If live threads
    // can no longer make progress (for example they only wait on events), a
    // RuntimeError identifies the deadlocked thread instead of leaving it
    // silently behind. Use `start` + `notify` for externally driven events.
    auto call(std::string_view function, std::span<Value const> args = {}) -> Value;
    auto call(std::string_view module, std::string_view function, std::span<Value const> args = {}) -> Value;
    auto call(FunctionRef const& function, std::span<Value const> args = {}) -> Value;

    // Explicit cooperative scheduling.
    auto start(std::string_view module, std::string_view function, std::span<Value const> args = {}) -> ThreadRef;
    auto start(FunctionRef const& function, std::span<Value const> args = {}) -> ThreadRef;
    auto thread_status(ThreadRef thread) const -> ThreadStatus;
    auto thread_result(ThreadRef thread) const -> Value;

    // Playground / embedding inspection. `threads()` includes terminal threads
    // as well as live ones. Values are expanded recursively according to
    // InspectOptions.
    auto thread_info(ThreadRef thread, InspectOptions options = {}) const -> ThreadInfo;
    auto threads(InspectOptions options = {}) const -> std::vector<ThreadInfo>;
    auto globals(InspectOptions options = {}) const -> std::vector<VariableInfo>;
    auto inspect_value(Value const& value, InspectOptions options = {}) const -> VariableInfo;
    auto memory_stats() const -> MemoryStats;
    auto memory_blocks(std::size_t preview_bytes = 64) const -> std::vector<MemoryBlockInfo>;

    // Instruction-level breakpoints and stepping. A breakpoint is identified by
    // module, script function and instruction index (`pc`, 0-based). `step`
    // executes exactly one instruction of the paused thread; `resume` runs until
    // the next breakpoint, all threads block, or all threads finish.
    auto set_breakpoint(std::string_view module, std::string_view function, std::size_t pc) -> bool;
    auto clear_breakpoints() -> void;
    auto breakpoints() const -> std::vector<BreakpointInfo>;
    auto debug_state() const -> DebugState;
    auto set_debug_enabled(bool enabled) -> void;
    auto step() -> std::size_t;
    auto resume() -> std::size_t;
    auto pause() -> void;

    // Run every runnable thread until all are blocked or finished. Returns the
    // number of instructions executed.
    auto run_until_idle() -> std::size_t;

    // Run all live threads to completion, advancing timed/frame waits. A set of
    // threads blocked only on events is a deadlock and raises RuntimeError.
    // Returns the number of instructions executed.
    auto run_until_complete() -> std::size_t;

    // Advance logical time, wake timed/frame waiters and run until idle. In
    // tick_based mode the delta is rounded up to a whole number of ticks.
    auto tick(double delta_seconds) -> std::size_t;

    // Dispatch an event to matching waiters. Returns the number of resumed or
    // terminated threads. The caller decides when to run them.
    auto notify(Value const& target, std::string_view event, std::span<Value const> args = {}) -> std::size_t;

    auto has_runnable_threads() const -> bool;
    auto has_live_threads() const -> bool;
    auto time() const -> double;

    auto set_instruction_limit(std::size_t limit) -> void;
    auto set_max_call_depth(std::size_t depth) -> void;
    auto set_execution_mode(ExecutionMode mode) -> void;
    auto set_tick_rate(double ticks_per_second) -> void;
    auto set_real_time(bool enabled) -> void;
    auto instruction_count() const -> std::size_t;

    // Engine singletons / globals. Level fields are the T6 global-variable
    // namespace; game is the engine-provided array-like object.
    auto level_object() -> ObjectPtr;
    auto game_object() -> ObjectPtr;
    auto world_object() -> ObjectPtr;
    auto classes_object() -> ObjectPtr;
    auto anim_object() -> ObjectPtr;

    auto get_global(std::string_view name) const -> Value;
    auto set_global(std::string_view name, Value value) -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gsc::t6
