// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/emscripten.h>

#include "gsc/gscvm.hpp"

namespace
{

using namespace gsc::t6;
using gsc::Error;

std::unique_ptr<VM> vm;
std::string output;
std::string last_error;

auto quote(std::string_view text) -> std::string
{
    auto out = std::string{ "\"" };
    out.reserve(text.size() + 2);
    for (auto const c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else
                    out.push_back(c);
                break;
        }
    }
    out.push_back('"');
    return out;
}

auto json_string(std::string const& value) -> std::string
{
    return quote(value);
}

auto json_bool(bool value) -> std::string
{
    return value ? "true" : "false";
}

auto make_string(std::string text) -> char*
{
    auto* result = static_cast<char*>(std::malloc(text.size() + 1));
    if (!result)
        return nullptr;
    std::memcpy(result, text.data(), text.size());
    result[text.size()] = '\0';
    return result;
}

auto error_json(std::string_view message) -> std::string
{
    return std::format("{{\"ok\":false,\"error\":{}}}", json_string(std::string{ message }));
}

auto ok_json() -> std::string
{
    return "{\"ok\":true}";
}

auto status_name(ThreadStatus status) -> std::string_view
{
    switch (status)
    {
        case ThreadStatus::unknown: return "unknown";
        case ThreadStatus::ready: return "ready";
        case ThreadStatus::running: return "running";
        case ThreadStatus::waiting_time: return "waiting_time";
        case ThreadStatus::waiting_frame: return "waiting_frame";
        case ThreadStatus::waiting_event: return "waiting_event";
        case ThreadStatus::finished: return "finished";
        case ThreadStatus::failed: return "failed";
        case ThreadStatus::aborted: return "aborted";
        case ThreadStatus::paused: return "paused";
    }
    return "unknown";
}

auto value_json(Value const& value) -> std::string
{
    return std::format("{{\"type\":{},\"value\":{}}}", json_string(std::string{ value.type_name() }), json_string(value.to_display_string()));
}

auto variable_json(VariableInfo const& value) -> std::string
{
    auto out = std::format("{{\"name\":{},\"type\":{},\"value\":{},\"expandable\":{},\"childCount\":{}",
        json_string(value.name), json_string(value.type), json_string(value.value), json_bool(value.expandable), value.child_count);

    if (!value.children.empty())
    {
        out += ",\"children\":[";
        for (std::size_t i = 0; i < value.children.size(); i++)
        {
            if (i != 0)
                out.push_back(',');
            out += variable_json(value.children[i]);
        }
        out.push_back(']');
    }

    out.push_back('}');
    return out;
}

auto variables_json(std::span<VariableInfo const> values) -> std::string
{
    auto out = std::string{ "[" };
    for (std::size_t i = 0; i < values.size(); i++)
    {
        if (i != 0)
            out.push_back(',');
        out += variable_json(values[i]);
    }
    out.push_back(']');
    return out;
}

auto frame_json(FrameInfo const& frame) -> std::string
{
    return std::format("{{\"module\":{},\"function\":{},\"pc\":{},\"offset\":{},\"opcode\":{},\"maxStack\":{},\"locals\":{},\"varargs\":{},\"stack\":{}}}",
        json_string(frame.module), json_string(frame.function), frame.pc, frame.offset, json_string(frame.opcode), frame.max_stack,
        variables_json(frame.locals), variables_json(frame.varargs), variables_json(frame.stack));
}

auto thread_json(ThreadInfo const& thread) -> std::string
{
    auto out = std::format("{{\"id\":{},\"status\":{},\"wakeTime\":{},\"waitEvent\":{},\"waitTarget\":{},\"result\":{},\"error\":{},\"endonCount\":{},\"frames\":[",
        thread.thread.id, json_string(std::string{ status_name(thread.status) }), thread.wake_time,
        json_string(thread.wait_event), json_string(thread.wait_target), json_string(thread.result), json_string(thread.error), thread.endon_count);

    for (std::size_t i = 0; i < thread.frames.size(); i++)
    {
        if (i != 0)
            out.push_back(',');
        out += frame_json(thread.frames[i]);
    }

    out += "]}";
    return out;
}

auto breakpoint_json(BreakpointInfo const& breakpoint) -> std::string
{
    return std::format("{{\"module\":{},\"function\":{},\"pc\":{}}}",
        json_string(breakpoint.module), json_string(breakpoint.function), breakpoint.pc);
}

auto debug_json(DebugState const& state) -> std::string
{
    auto out = std::format("{{\"enabled\":{},\"paused\":{},\"thread\":{},\"module\":{},\"function\":{},\"pc\":{},\"offset\":{},\"opcode\":{},\"breakpoints\":[",
        json_bool(state.enabled), json_bool(state.paused), state.thread.id, json_string(state.module), json_string(state.function),
        state.pc, state.offset, json_string(state.opcode));

    for (std::size_t i = 0; i < state.breakpoints.size(); i++)
    {
        if (i != 0)
            out.push_back(',');
        out += breakpoint_json(state.breakpoints[i]);
    }

    out += "]}";
    return out;
}

auto memory_stats_json(MemoryStats const& stats) -> std::string
{
    return std::format("{{\"modules\":{},\"functions\":{},\"instructions\":{},\"moduleBytes\":{},\"threads\":{},\"liveThreads\":{},\"frames\":{},\"locals\":{},\"stackItems\":{},\"arrays\":{},\"objects\":{},\"values\":{},\"nestedValues\":{},\"estimatedBytes\":{}}}",
        stats.modules, stats.functions, stats.instructions, stats.module_bytes, stats.threads, stats.live_threads,
        stats.frames, stats.locals, stats.stack_items, stats.arrays, stats.objects, stats.values, stats.nested_values, stats.estimated_bytes);
}

auto memory_block_json(MemoryBlockInfo const& block) -> std::string
{
    return std::format("{{\"kind\":{},\"name\":{},\"size\":{},\"previewHex\":{}}}",
        json_string(block.kind), json_string(block.name), block.size, json_string(block.preview_hex));
}

auto module_json(ModuleInfo const& info) -> std::string
{
    auto out = std::format("{{\"name\":{},\"includes\":[", json_string(info.name));
    for (std::size_t i = 0; i < info.includes.size(); i++)
    {
        if (i != 0)
            out.push_back(',');
        out += json_string(info.includes[i]);
    }
    out += "],\"functions\":[";
    for (std::size_t i = 0; i < info.functions.size(); i++)
    {
        auto const& function = info.functions[i];
        if (i != 0)
            out.push_back(',');
        out += std::format("{{\"name\":{},\"space\":{},\"params\":{},\"flags\":{},\"offset\":{},\"size\":{}}}",
            json_string(function.name), json_string(function.space), function.params, function.flags, function.offset, function.size);
    }
    out += "]}";
    return out;
}

auto result_json(std::string body) -> char*
{
    return make_string(std::move(body));
}

auto error_result(std::exception const& error) -> char*
{
    last_error = error.what();
    return make_string(error_json(error.what()));
}

auto require_vm() -> VM&
{
    if (!vm)
        throw Error("gscvm is not initialized");
    return *vm;
}

class PlaygroundHost final : public Host
{
public:
    auto log(LogLevel level, std::string_view message) -> void override
    {
        output += "[";
        output.append(log_level_name(level));
        output += "] ";
        output.append(message);
        output.push_back('\n');
    }
};

auto register_print(std::string_view name, bool newline) -> void
{
    require_vm().natives().register_function("", name, [newline](NativeContext& context) {
        auto const format = context.argument_count() == 0 ? std::string{} : context.argument(0).to_string();
        auto arguments = std::vector<Value>{};
        for (std::size_t i = 1; i < context.argument_count(); i++)
            arguments.push_back(context.argument(i));
        output += format_message(format, arguments);
        if (newline)
            output.push_back('\n');
    });
}

auto create_vm() -> void
{
    auto config = VMConfig{};
    config.real_time = false;
    config.execution_mode = ExecutionMode::real_time;
    config.host = std::make_shared<PlaygroundHost>();
    vm = std::make_unique<VM>(config);
    output.clear();
    last_error.clear();

    register_print("print", false);
    register_print("println", true);
    register_print("iprintln", true);
    register_print("iprintlnbold", true);

    vm->natives().register_function("", "gettime", [](NativeContext& context) {
        context.set_return_value(Value::floating(static_cast<float>(context.vm().time())));
    });
}

} // namespace

extern "C"
{

EMSCRIPTEN_KEEPALIVE int gscvm_init()
{
    try
    {
        create_vm();
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE void gscvm_shutdown()
{
    vm.reset();
    output.clear();
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_real_time(int enabled)
{
    try
    {
        require_vm().set_real_time(enabled != 0);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_tick_rate(double ticks_per_second)
{
    try
    {
        require_vm().set_tick_rate(ticks_per_second);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_execution_mode(int mode)
{
    try
    {
        require_vm().set_execution_mode(mode == 1 ? ExecutionMode::tick_based : ExecutionMode::real_time);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_instruction_limit(unsigned int limit)
{
    try
    {
        require_vm().set_instruction_limit(limit);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_add_search_path(const char* path)
{
    try
    {
        if (!path)
            return 0;
        require_vm().add_search_path(std::filesystem::path{ path });
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE void gscvm_clear_search_paths()
{
    try
    {
        require_vm().clear_search_paths();
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_add_file(const char* path, const char* data, int size)
{
    try
    {
        if (!path || !data || size < 0)
            return 0;
        auto const bytes = std::span<std::uint8_t const>{ reinterpret_cast<std::uint8_t const*>(data), static_cast<std::size_t>(size) };
        require_vm().add_virtual_file(path, bytes);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_add_source(const char* path, const char* source)
{
    try
    {
        if (!path || !source)
            return 0;
        require_vm().add_virtual_file(path, std::string_view{ source });
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_remove_file(const char* path)
{
    try
    {
        if (!path)
            return 0;
        return require_vm().remove_virtual_file(path) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE void gscvm_clear_files()
{
    try
    {
        require_vm().clear_virtual_files();
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_list_files()
{
    try
    {
        auto const files = require_vm().virtual_files();
        auto out = std::string{ "[" };
        for (std::size_t i = 0; i < files.size(); i++)
        {
            if (i != 0)
                out.push_back(',');
            out += json_string(files[i]);
        }
        out.push_back(']');
        return result_json(std::move(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_resolve_path(const char* path)
{
    try
    {
        if (!path)
            return nullptr;
        auto const resolved = require_vm().resolve_virtual_path(path);
        if (!resolved)
            return nullptr;
        return make_string(*resolved);
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return nullptr;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_load_file(const char* path, const char* name)
{
    try
    {
        if (!path)
            return result_json(error_json("missing path"));
        auto module_name = std::optional<std::string>{};
        if (name && *name)
            module_name = name;
        auto const info = require_vm().load_module_file(path, module_name);
        return result_json(std::format("{{\"ok\":true,\"module\":{}}}", module_json(info)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_load_source(const char* name, const char* source)
{
    try
    {
        if (!name || !source)
            return result_json(error_json("missing name or source"));
        auto const text = std::string_view{ source };
        auto const bytes = std::span<std::uint8_t const>{ reinterpret_cast<std::uint8_t const*>(text.data()), text.size() };
        auto const info = require_vm().load_module(name, bytes);
        return result_json(std::format("{{\"ok\":true,\"module\":{}}}", module_json(info)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_load_module(const char* name, const char* data, int size)
{
    try
    {
        if (!name || !data || size < 0)
            return result_json(error_json("missing name or data"));
        auto const bytes = std::span<std::uint8_t const>{ reinterpret_cast<std::uint8_t const*>(data), static_cast<std::size_t>(size) };
        auto const info = require_vm().load_module(name, bytes);
        return result_json(std::format("{{\"ok\":true,\"module\":{}}}", module_json(info)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_load_dependencies(const char* module)
{
    try
    {
        if (!module)
            return result_json(error_json("missing module"));
        require_vm().load_dependencies(module);
        return result_json(ok_json());
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_call(const char* module, const char* function)
{
    try
    {
        if (!function)
            return result_json(error_json("missing function"));
        auto value = module && *module ? require_vm().call(module, function) : require_vm().call(function);
        return result_json(std::format("{{\"ok\":true,\"result\":{}}}", value_json(value)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_start(const char* module, const char* function)
{
    try
    {
        if (!function)
            return result_json(error_json("missing function"));
        auto const thread = module && *module ? require_vm().start(module, function) : require_vm().start(std::string_view{}, function);
        return result_json(std::format("{{\"ok\":true,\"thread\":{}}}", thread.id));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_tick(double delta_seconds)
{
    try
    {
        auto const executed = require_vm().tick(delta_seconds);
        return result_json(std::format("{{\"ok\":true,\"executed\":{},\"time\":{},\"debug\":{}}}",
            executed, require_vm().time(), debug_json(require_vm().debug_state())));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_run_until_idle()
{
    try
    {
        auto const executed = require_vm().run_until_idle();
        return result_json(std::format("{{\"ok\":true,\"executed\":{},\"debug\":{}}}", executed, debug_json(require_vm().debug_state())));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_run_until_complete()
{
    try
    {
        auto const executed = require_vm().run_until_complete();
        return result_json(std::format("{{\"ok\":true,\"executed\":{},\"debug\":{}}}", executed, debug_json(require_vm().debug_state())));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_notify(const char* event)
{
    try
    {
        if (!event)
            return result_json(error_json("missing event"));
        auto const count = require_vm().notify(Value::object(require_vm().level_object()), event);
        return result_json(std::format("{{\"ok\":true,\"resumed\":{}}}", count));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_threads()
{
    try
    {
        auto const threads = require_vm().threads();
        auto out = std::string{ "[" };
        for (std::size_t i = 0; i < threads.size(); i++)
        {
            if (i != 0)
                out.push_back(',');
            out += thread_json(threads[i]);
        }
        out.push_back(']');
        return result_json(std::move(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_thread_info(unsigned int id)
{
    try
    {
        return result_json(thread_json(require_vm().thread_info(ThreadRef{ id })));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_globals()
{
    try
    {
        return result_json(variables_json(require_vm().globals()));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_memory_stats()
{
    try
    {
        return result_json(memory_stats_json(require_vm().memory_stats()));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_memory_blocks(unsigned int preview_bytes)
{
    try
    {
        auto const blocks = require_vm().memory_blocks(preview_bytes);
        auto out = std::string{ "[" };
        for (std::size_t i = 0; i < blocks.size(); i++)
        {
            if (i != 0)
                out.push_back(',');
            out += memory_block_json(blocks[i]);
        }
        out.push_back(']');
        return result_json(std::move(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_modules()
{
    try
    {
        auto const modules = require_vm().modules();
        auto out = std::string{ "[" };
        for (std::size_t i = 0; i < modules.size(); i++)
        {
            if (i != 0)
                out.push_back(',');
            out += module_json(modules[i]);
        }
        out.push_back(']');
        return result_json(std::move(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_disassembly(const char* name)
{
    try
    {
        if (!name)
            return nullptr;
        return make_string(require_vm().disassembly(name));
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return nullptr;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_module_bytes(const char* name)
{
    try
    {
        if (!name)
            return nullptr;
        auto const bytes = require_vm().module_bytes(name);
        auto out = std::string{};
        static constexpr char digits[] = "0123456789abcdef";
        out.reserve(bytes.size() * 2);
        for (auto const byte : bytes)
        {
            out.push_back(digits[byte >> 4]);
            out.push_back(digits[byte & 0x0f]);
        }
        return make_string(std::move(out));
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return nullptr;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_set_breakpoint(const char* module, const char* function, unsigned int pc)
{
    try
    {
        if (!function)
            return result_json(error_json("missing function"));
        auto const ok = require_vm().set_breakpoint(module ? module : "", function, pc);
        return result_json(std::format("{{\"ok\":{},\"debug\":{}}}", json_bool(ok), debug_json(require_vm().debug_state())));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_clear_breakpoints()
{
    try
    {
        require_vm().clear_breakpoints();
        return result_json(ok_json());
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_breakpoints()
{
    try
    {
        auto const breakpoints = require_vm().breakpoints();
        auto out = std::string{ "[" };
        for (std::size_t i = 0; i < breakpoints.size(); i++)
        {
            if (i != 0)
                out.push_back(',');
            out += breakpoint_json(breakpoints[i]);
        }
        out.push_back(']');
        return result_json(std::move(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_debug_state()
{
    try
    {
        return result_json(debug_json(require_vm().debug_state()));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_step()
{
    try
    {
        auto const executed = require_vm().step();
        return result_json(std::format("{{\"ok\":true,\"executed\":{},\"debug\":{}}}", executed, debug_json(require_vm().debug_state())));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_resume()
{
    try
    {
        auto const executed = require_vm().resume();
        return result_json(std::format("{{\"ok\":true,\"executed\":{},\"debug\":{}}}", executed, debug_json(require_vm().debug_state())));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_pause()
{
    try
    {
        require_vm().pause();
        return result_json(ok_json());
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_take_output()
{
    auto out = std::move(output);
    output.clear();
    return make_string(std::move(out));
}

EMSCRIPTEN_KEEPALIVE const char* gscvm_last_error()
{
    return last_error.c_str();
}

EMSCRIPTEN_KEEPALIVE void gscvm_free(void* pointer)
{
    std::free(pointer);
}

} // extern "C"
