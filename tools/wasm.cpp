// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/emscripten.h>

#include "gsc/gscvm.hpp"

#include "wasm_bridge.hpp"
#include "wasm_host.hpp"
#include "wasm_json.hpp"
#include "wasm_value.hpp"

namespace
{

using namespace gsc::t6;
using gsc::Error;
using gsc::wasm::Json;
using gsc::wasm::WasmHost;

std::unique_ptr<VM> vm;
std::string last_error;
int pending_host_id{};

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
    return std::format("{{\"ok\":false,\"error\":{}}}", gsc::wasm::json_quote(message));
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
    return std::format("{{\"type\":{},\"value\":{}}}", gsc::wasm::json_quote(value.type_name()), gsc::wasm::json_quote(value.to_display_string()));
}

auto variable_json(VariableInfo const& value) -> std::string
{
    auto out = std::format("{{\"name\":{},\"type\":{},\"value\":{},\"expandable\":{},\"childCount\":{}",
        gsc::wasm::json_quote(value.name), gsc::wasm::json_quote(value.type), gsc::wasm::json_quote(value.value), value.expandable ? "true" : "false", value.child_count);

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
        gsc::wasm::json_quote(frame.module), gsc::wasm::json_quote(frame.function), frame.pc, frame.offset, gsc::wasm::json_quote(frame.opcode), frame.max_stack,
        variables_json(frame.locals), variables_json(frame.varargs), variables_json(frame.stack));
}

auto thread_json(ThreadInfo const& thread) -> std::string
{
    auto out = std::format("{{\"id\":{},\"status\":{},\"wakeTime\":{},\"waitEvent\":{},\"waitTarget\":{},\"result\":{},\"error\":{},\"endonCount\":{},\"frames\":[",
        thread.thread.id, gsc::wasm::json_quote(status_name(thread.status)), thread.wake_time,
        gsc::wasm::json_quote(thread.wait_event), gsc::wasm::json_quote(thread.wait_target), gsc::wasm::json_quote(thread.result), gsc::wasm::json_quote(thread.error), thread.endon_count);

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
        gsc::wasm::json_quote(breakpoint.module), gsc::wasm::json_quote(breakpoint.function), breakpoint.pc);
}

auto debug_json(DebugState const& state) -> std::string
{
    auto out = std::format("{{\"enabled\":{},\"paused\":{},\"thread\":{},\"module\":{},\"function\":{},\"pc\":{},\"offset\":{},\"opcode\":{},\"breakpoints\":[",
        state.enabled ? "true" : "false", state.paused ? "true" : "false", state.thread.id, gsc::wasm::json_quote(state.module), gsc::wasm::json_quote(state.function),
        state.pc, state.offset, gsc::wasm::json_quote(state.opcode));

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
        gsc::wasm::json_quote(block.kind), gsc::wasm::json_quote(block.name), block.size, gsc::wasm::json_quote(block.preview_hex));
}

auto module_json(ModuleInfo const& info) -> std::string
{
    auto out = std::format("{{\"name\":{},\"includes\":[", gsc::wasm::json_quote(info.name));
    for (std::size_t i = 0; i < info.includes.size(); i++)
    {
        if (i != 0)
            out.push_back(',');
        out += gsc::wasm::json_quote(info.includes[i]);
    }
    out += "],\"functions\":[";
    for (std::size_t i = 0; i < info.functions.size(); i++)
    {
        auto const& function = info.functions[i];
        if (i != 0)
            out.push_back(',');
        out += std::format("{{\"name\":{},\"space\":{},\"params\":{},\"flags\":{},\"offset\":{},\"size\":{}}}",
            gsc::wasm::json_quote(function.name), gsc::wasm::json_quote(function.space), function.params, function.flags, function.offset, function.size);
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

auto wasm_host() -> WasmHost*
{
    if (!vm)
        return nullptr;
    return dynamic_cast<WasmHost*>(&vm->host());
}

auto append_output(std::string_view text) -> void
{
    if (auto* host = wasm_host())
        host->append_output(text);
}

auto config_json(VMConfig const& config) -> std::string
{
    auto out = Json::make_object();
    out.set("instance", Json{ config.instance == Instance::client ? "client" : "server" });
    out.set("executionMode", Json{ config.execution_mode == ExecutionMode::tick_based ? "tick_based" : "real_time" });
    out.set("tickRate", Json{ config.tick_rate });
    out.set("frameTime", Json{ config.frame_time });
    out.set("realTime", Json{ config.real_time });
    out.set("instructionLimit", Json{ static_cast<double>(config.instruction_limit) });
    out.set("maxCallDepth", Json{ static_cast<double>(config.max_call_depth) });
    out.set("maxStackSize", Json{ static_cast<double>(config.max_stack_size) });
    out.set("maxThreads", Json{ static_cast<double>(config.max_threads) });
    out.set("executeDevblocks", Json{ config.execute_devblocks });
    out.set("t6Fixup", Json{ config.t6_fixup });
    out.set("hostId", Json{ pending_host_id });
    auto paths = Json::make_array();
    for (auto const& path : config.search_paths)
        paths.append(Json{ path.generic_string() });
    out.set("searchPaths", std::move(paths));
    return gsc::wasm::stringify_json(out);
}

auto apply_config(VMConfig& config, Json const& value) -> void
{
    if (!value.is_object())
        throw Error("configuration must be a JSON object");

    if (auto const* field = value.find("instance"))
    {
        if (field->is_string())
            config.instance = field->string == "client" ? Instance::client : Instance::server;
        else if (field->is_number())
            config.instance = static_cast<int>(field->number) == 1 ? Instance::client : Instance::server;
    }
    if (auto const* field = value.find("executionMode"))
    {
        if (field->is_string())
            config.execution_mode = field->string == "tick_based" || field->string == "tick" ? ExecutionMode::tick_based : ExecutionMode::real_time;
        else if (field->is_number())
            config.execution_mode = static_cast<int>(field->number) == 1 ? ExecutionMode::tick_based : ExecutionMode::real_time;
    }
    if (auto const* field = value.find("tickRate"); field && field->is_number())
        config.tick_rate = field->number;
    if (auto const* field = value.find("frameTime"); field && field->is_number())
        config.frame_time = field->number;
    if (auto const* field = value.find("realTime"); field && field->is_boolean())
        config.real_time = field->boolean;
    if (auto const* field = value.find("instructionLimit"); field && field->is_number())
        config.instruction_limit = static_cast<std::size_t>(field->number);
    if (auto const* field = value.find("maxCallDepth"); field && field->is_number())
        config.max_call_depth = static_cast<std::size_t>(field->number);
    if (auto const* field = value.find("maxStackSize"); field && field->is_number())
        config.max_stack_size = static_cast<std::size_t>(field->number);
    if (auto const* field = value.find("maxThreads"); field && field->is_number())
        config.max_threads = static_cast<std::size_t>(field->number);
    if (auto const* field = value.find("executeDevblocks"); field && field->is_boolean())
        config.execute_devblocks = field->boolean;
    if (auto const* field = value.find("t6Fixup"); field && field->is_boolean())
        config.t6_fixup = field->boolean;
    if (auto const* field = value.find("searchPaths"); field && field->is_array())
    {
        config.search_paths.clear();
        for (auto const& path : field->array)
        {
            if (!path.is_string())
                throw Error("searchPaths entries must be strings");
            config.search_paths.emplace_back(path.string);
        }
    }
}

auto object_for_kind(std::string_view kind) -> ObjectPtr
{
    auto& instance = require_vm();
    if (kind == "level")
        return instance.level_object();
    if (kind == "game")
        return instance.game_object();
    if (kind == "world")
        return instance.world_object();
    if (kind == "classes")
        return instance.classes_object();
    if (kind == "anim")
        return instance.anim_object();
    throw Error(std::format("unknown object '{}'", kind));
}

auto values_span(std::vector<Value> const& values) -> std::span<Value const>
{
    return std::span<Value const>{ values.data(), values.size() };
}

auto register_print(std::string_view name, bool newline) -> void
{
    require_vm().natives().register_function("", name, [newline](NativeContext& context) {
        auto const format = context.argument_count() == 0 ? std::string{} : context.argument(0).to_string();
        auto arguments = std::vector<Value>{};
        for (std::size_t i = 1; i < context.argument_count(); i++)
            arguments.push_back(context.argument(i));
        append_output(format_message(format, arguments));
        if (newline)
            append_output("\n");
    });
}

auto register_js_native(std::string_view space, std::string_view name, int callback_id) -> void
{
    require_vm().natives().register_function(space, name, [callback_id](NativeContext& context) {
        auto meta = Json::make_object();
        meta.set("space", Json{ std::string{ context.function_space() } });
        meta.set("name", Json{ std::string{ context.function_name() } });
        meta.set("threadId", Json{ static_cast<double>(context.thread_id()) });
        meta.set("self", gsc::wasm::encode_value(context.self()));

        auto args = std::vector<Value>{};
        args.reserve(context.argument_count());
        for (std::size_t i = 0; i < context.argument_count(); i++)
            args.push_back(context.argument(i));

        auto result = gsc::wasm::js_native_call(
            callback_id,
            gsc::wasm::stringify_json(meta),
            gsc::wasm::stringify_json(gsc::wasm::encode_values(values_span(args))));

        if (result.empty())
        {
            context.return_undefined();
            return;
        }

        auto value = gsc::wasm::parse_json(result);
        if (value.is_object())
        {
            if (auto const* error = value.find("$gscError"); error && error->is_string())
                context.error(error->string);
        }
        context.set_return_value(gsc::wasm::decode_value(value));
    });
}

auto create_vm(Json const* options) -> void
{
    auto config = VMConfig{};
    config.real_time = false;
    config.execution_mode = ExecutionMode::real_time;
    if (options)
        apply_config(config, *options);

    auto host = std::make_shared<WasmHost>(pending_host_id);
    config.host = host;

    vm = std::make_unique<VM>(std::move(config));
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

int gscvm_init_ex(const char* config_text);

EMSCRIPTEN_KEEPALIVE int gscvm_init()
{
    return gscvm_init_ex(nullptr);
}

EMSCRIPTEN_KEEPALIVE int gscvm_init_ex(const char* config_text)
{
    try
    {
        auto options = std::optional<Json>{};
        if (config_text && *config_text)
            options = gsc::wasm::parse_json(config_text);
        create_vm(options ? &*options : nullptr);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        vm.reset();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE void gscvm_shutdown()
{
    vm.reset();
    pending_host_id = 0;
    last_error.clear();
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_js_host(int host_id)
{
    try
    {
        pending_host_id = host_id;
        if (vm)
        {
            auto* host = dynamic_cast<WasmHost*>(&vm->host());
            if (host)
            {
                host->set_host_id(host_id);
                vm->set_host(vm->config().host);
            }
        }
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_config()
{
    try
    {
        return result_json(config_json(require_vm().config()));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_instance(int client)
{
    try
    {
        require_vm().set_instance(client != 0 ? Instance::client : Instance::server);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_frame_time(double seconds)
{
    try
    {
        require_vm().set_frame_time(seconds);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
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

EMSCRIPTEN_KEEPALIVE int gscvm_set_max_call_depth(unsigned int depth)
{
    try
    {
        require_vm().set_max_call_depth(depth);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_max_stack_size(unsigned int size)
{
    try
    {
        require_vm().set_max_stack_size(size);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_max_threads(unsigned int count)
{
    try
    {
        require_vm().set_max_threads(count);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_execute_devblocks(int enabled)
{
    try
    {
        require_vm().set_execute_devblocks(enabled != 0);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_t6_fixup(int enabled)
{
    try
    {
        require_vm().set_t6_fixup(enabled != 0);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_debug_enabled(int enabled)
{
    try
    {
        require_vm().set_debug_enabled(enabled != 0);
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

EMSCRIPTEN_KEEPALIVE char* gscvm_search_paths()
{
    try
    {
        auto const paths = require_vm().search_paths();
        auto out = Json::make_array();
        for (auto const& path : paths)
            out.append(Json{ path.generic_string() });
        return result_json(gsc::wasm::stringify_json(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
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
        auto out = Json::make_array();
        for (auto const& file : files)
            out.append(Json{ file });
        return result_json(gsc::wasm::stringify_json(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_read_file(const char* path)
{
    try
    {
        if (!path)
            return result_json(error_json("missing path"));
        auto const data = require_vm().read_virtual_file(path);
        if (!data)
            return result_json(error_json(std::format("unknown file '{}'", path)));
        auto bytes = Json::make_array();
        for (auto const byte : *data)
            bytes.append(Json{ static_cast<double>(byte) });
        auto out = Json::make_object();
        out.set("ok", Json{ true });
        out.set("path", Json{ std::string{ path } });
        out.set("data", std::move(bytes));
        return result_json(gsc::wasm::stringify_json(out));
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

EMSCRIPTEN_KEEPALIVE int gscvm_has_module(const char* module)
{
    try
    {
        if (!module)
            return 0;
        return require_vm().has_module(module) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_module_info(const char* module)
{
    try
    {
        if (!module)
            return result_json(error_json("missing module"));
        return result_json(module_json(require_vm().module_info(module)));
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

EMSCRIPTEN_KEEPALIVE char* gscvm_call_values(const char* module, const char* function, const char* args_json)
{
    try
    {
        if (!function)
            return result_json(error_json("missing function"));
        auto args = args_json && *args_json ? gsc::wasm::values_from_json(args_json) : std::vector<Value>{};
        auto value = module && *module
            ? require_vm().call(module, function, values_span(args))
            : require_vm().call(function, values_span(args));
        return result_json(std::format("{{\"ok\":true,\"result\":{}}}", gsc::wasm::value_json(value)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_call_function(const char* function_json, const char* args_json)
{
    try
    {
        if (!function_json)
            return result_json(error_json("missing function"));
        auto function = gsc::wasm::value_from_json(function_json);
        if (!function.is_function())
            return result_json(error_json("value is not a function"));
        auto args = args_json && *args_json ? gsc::wasm::values_from_json(args_json) : std::vector<Value>{};
        auto value = require_vm().call(function.as_function(), values_span(args));
        return result_json(std::format("{{\"ok\":true,\"result\":{}}}", gsc::wasm::value_json(value)));
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

EMSCRIPTEN_KEEPALIVE char* gscvm_start_values(const char* module, const char* function, const char* args_json)
{
    try
    {
        if (!function)
            return result_json(error_json("missing function"));
        auto args = args_json && *args_json ? gsc::wasm::values_from_json(args_json) : std::vector<Value>{};
        auto const thread = module && *module
            ? require_vm().start(module, function, values_span(args))
            : require_vm().start(module ? module : "", function, values_span(args));
        return result_json(std::format("{{\"ok\":true,\"thread\":{}}}", thread.id));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_start_function(const char* function_json, const char* args_json)
{
    try
    {
        if (!function_json)
            return result_json(error_json("missing function"));
        auto function = gsc::wasm::value_from_json(function_json);
        if (!function.is_function())
            return result_json(error_json("value is not a function"));
        auto args = args_json && *args_json ? gsc::wasm::values_from_json(args_json) : std::vector<Value>{};
        auto const thread = require_vm().start(function.as_function(), values_span(args));
        return result_json(std::format("{{\"ok\":true,\"thread\":{}}}", thread.id));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_thread_status(unsigned int id)
{
    try
    {
        auto out = Json::make_object();
        out.set("ok", Json{ true });
        out.set("status", Json{ std::string{ status_name(require_vm().thread_status(ThreadRef{ id })) } });
        return result_json(gsc::wasm::stringify_json(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_thread_result_values(unsigned int id)
{
    try
    {
        return result_json(gsc::wasm::value_json(require_vm().thread_result(ThreadRef{ id })));
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

EMSCRIPTEN_KEEPALIVE char* gscvm_notify_values(const char* target_json, const char* event, const char* args_json)
{
    try
    {
        if (!event)
            return result_json(error_json("missing event"));
        auto target = target_json && *target_json ? gsc::wasm::value_from_json(target_json) : Value::object(require_vm().level_object());
        auto args = args_json && *args_json ? gsc::wasm::values_from_json(args_json) : std::vector<Value>{};
        auto const count = require_vm().notify(target, event, values_span(args));
        return result_json(std::format("{{\"ok\":true,\"resumed\":{}}}", count));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE double gscvm_time()
{
    return vm ? vm->time() : 0.0;
}

EMSCRIPTEN_KEEPALIVE double gscvm_instruction_count()
{
    return vm ? static_cast<double>(vm->instruction_count()) : 0.0;
}

EMSCRIPTEN_KEEPALIVE int gscvm_has_runnable_threads()
{
    return vm && vm->has_runnable_threads() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int gscvm_has_live_threads()
{
    return vm && vm->has_live_threads() ? 1 : 0;
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

EMSCRIPTEN_KEEPALIVE char* gscvm_inspect_value(const char* value_json_text, unsigned int max_depth, unsigned int max_items)
{
    try
    {
        if (!value_json_text)
            return result_json(error_json("missing value"));
        auto options = InspectOptions{};
        if (max_depth != 0)
            options.max_depth = max_depth;
        if (max_items != 0)
            options.max_items = max_items;
        auto const value = gsc::wasm::value_from_json(value_json_text);
        return result_json(variable_json(require_vm().inspect_value(value, options)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_get_global(const char* name)
{
    try
    {
        if (!name)
            return result_json(error_json("missing name"));
        return result_json(gsc::wasm::value_json(require_vm().get_global(name)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_global(const char* name, const char* value_json_text)
{
    try
    {
        if (!name || !value_json_text)
            return 0;
        require_vm().set_global(name, gsc::wasm::value_from_json(value_json_text));
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_object(const char* kind)
{
    try
    {
        if (!kind)
            return result_json(error_json("missing kind"));
        return result_json(gsc::wasm::value_json(Value::object(object_for_kind(kind))));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_object_get_field(const char* kind, const char* name)
{
    try
    {
        if (!kind || !name)
            return result_json(error_json("missing kind or name"));
        auto object = object_for_kind(kind);
        return result_json(gsc::wasm::value_json(object->get_field(name)));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_object_set_field(const char* kind, const char* name, const char* value_json_text)
{
    try
    {
        if (!kind || !name || !value_json_text)
            return 0;
        object_for_kind(kind)->set_field(name, gsc::wasm::value_from_json(value_json_text));
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_object_clear_field(const char* kind, const char* name)
{
    try
    {
        if (!kind || !name)
            return 0;
        return object_for_kind(kind)->clear_field(name) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_object_has_field(const char* kind, const char* name)
{
    try
    {
        if (!kind || !name)
            return 0;
        return object_for_kind(kind)->has_field(name) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_object_keys(const char* kind)
{
    try
    {
        if (!kind)
            return result_json(error_json("missing kind"));
        auto const keys = object_for_kind(kind)->keys();
        return result_json(std::format("{{\"ok\":true,\"keys\":{}}}", gsc::wasm::stringify_json(gsc::wasm::encode_values(keys))));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_object_get_index(const char* kind, const char* key_json)
{
    try
    {
        if (!kind || !key_json)
            return result_json(error_json("missing kind or key"));
        auto object = object_for_kind(kind);
        return result_json(gsc::wasm::value_json(object->get_index(gsc::wasm::value_from_json(key_json))));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_object_set_index(const char* kind, const char* key_json, const char* value_json_text)
{
    try
    {
        if (!kind || !key_json || !value_json_text)
            return 0;
        object_for_kind(kind)->set_index(gsc::wasm::value_from_json(key_json), gsc::wasm::value_from_json(value_json_text));
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_object_clear_index(const char* kind, const char* key_json)
{
    try
    {
        if (!kind || !key_json)
            return 0;
        return object_for_kind(kind)->clear_index(gsc::wasm::value_from_json(key_json)) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_object_size(const char* kind)
{
    try
    {
        if (!kind)
            return 0;
        return static_cast<int>(object_for_kind(kind)->size());
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_register_js_native(const char* space, const char* name, int callback_id)
{
    try
    {
        if (!name || callback_id <= 0)
            return 0;
        register_js_native(space ? space : "", name, callback_id);
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_unregister_native(const char* space, const char* name)
{
    try
    {
        if (!name)
            return 0;
        return require_vm().natives().unregister_function(space ? space : "", name) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_has_native(const char* space, const char* name)
{
    try
    {
        if (!name)
            return 0;
        return require_vm().natives().contains(space ? space : "", name) ? 1 : 0;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE char* gscvm_natives()
{
    try
    {
        auto out = Json::make_array();
        for (auto const& name : require_vm().natives().names())
            out.append(Json{ name });
        return result_json(gsc::wasm::stringify_json(out));
    }
    catch (std::exception const& error)
    {
        return error_result(error);
    }
}

EMSCRIPTEN_KEEPALIVE int gscvm_set_dvar(const char* name, const char* value_json_text)
{
    try
    {
        if (!name || !value_json_text)
            return 0;
        auto* host = wasm_host();
        if (!host)
            throw Error("default host is unavailable");
        host->set_dvar(name, gsc::wasm::value_from_json(value_json_text));
        return 1;
    }
    catch (std::exception const& error)
    {
        last_error = error.what();
        return 0;
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
        return result_json(std::format("{{\"ok\":{},\"debug\":{}}}", ok ? "true" : "false", debug_json(require_vm().debug_state())));
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
    auto out = std::string{};
    if (auto* host = wasm_host())
        out = host->take_output();
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
