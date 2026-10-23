// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gsc/gscvm.hpp"

namespace
{

using namespace gsc::t6;

auto parse_value(std::string_view text) -> Value
{
    if (text == "undefined" || text == "nil")
        return Value::undefined();
    if (text == "true")
        return Value::integer(1);
    if (text == "false")
        return Value::integer(0);

    if (text.starts_with("\"") && text.ends_with("\"") && text.size() >= 2)
        return Value::string(std::string{ text.substr(1, text.size() - 2) });

    auto integer = std::int32_t{};
    if (auto const parsed = std::from_chars(text.data(), text.data() + text.size(), integer, 0); parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
        return Value::integer(integer);

    auto floating = 0.0;
    try
    {
        std::size_t consumed = 0;
        floating = std::stod(std::string{ text }, &consumed);
        if (consumed == text.size())
            return Value::floating(static_cast<float>(floating));
    }
    catch (...)
    {
    }

    return Value::string(std::string{ text });
}

auto print_usage() -> void
{
    std::cout <<
        "usage: gscvm [options] <script> [function [args...]]\n"
        "\n"
        "Accepts compiled T6 scripts or .gsc/.csc source. Source files are\n"
        "compiled automatically. The entry point defaults to main.\n"
        "\n"
        "options:\n"
        "  --info             print module information\n"
        "  --list             list exported functions\n"
        "  --disasm           print the disassembly\n"
        "  --search <path>    add a script search path (repeatable)\n"
        "  --client           use the client (rather than server) T6 profile\n"
        "  --t6fixup          enable the upstream T6 header fixup\n"
        "  --devblocks        execute developer blocks\n"
        "  --limit <n>        instruction limit per run (0 disables)\n"
        "  --tick-rate <hz>   use tick-based scheduling at the given rate\n"
        "  --frames <n>       run n scheduler ticks after the call\n"
        "  --time <seconds>   advance the clock by this amount per tick\n"
        "  --notify <event> <target> [args...]\n"
        "  --help             show this message\n";
}

auto print_module(ModuleInfo const& info, bool list) -> void
{
    std::cout << std::format("module: {}\n", info.name);
    if (!info.includes.empty())
    {
        std::cout << "includes:";
        for (auto const& include : info.includes)
            std::cout << ' ' << include;
        std::cout << '\n';
    }

    if (!list && !info.functions.empty())
        std::cout << std::format("functions: {}\n", info.functions.size());

    for (auto const& function : info.functions)
    {
        if (list)
            std::cout << std::format("  {}{} ({} bytes, {} params)\n", function.space.empty() ? "" : function.space + "::", function.name, function.size, function.params);
    }
}

} // namespace

int main(int argc, char** argv)
{
    auto arguments = std::vector<std::string>{ argv + 1, argv + argc };
    auto search_paths = std::vector<std::filesystem::path>{};
    auto function_name = std::optional<std::string>{};
    auto script_path = std::optional<std::filesystem::path>{};
    auto call_args = std::vector<std::string>{};
    auto notify_event = std::optional<std::string>{};
    auto notify_target = std::optional<std::string>{};
    auto notify_args = std::vector<std::string>{};

    auto show_info = false;
    auto show_list = false;
    auto show_disasm = false;
    auto client = false;
    auto fixup = false;
    auto devblocks = false;
    auto limit = std::optional<std::size_t>{};
    auto tick_rate = std::optional<double>{};
    auto frames = std::size_t{ 0 };
    auto tick_time = 0.0;

    for (std::size_t i = 0; i < arguments.size(); i++)
    {
        auto const& arg = arguments[i];

        if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
        else if (arg == "--info")
            show_info = true;
        else if (arg == "--list")
            show_list = true;
        else if (arg == "--disasm")
            show_disasm = true;
        else if (arg == "--client")
            client = true;
        else if (arg == "--t6fixup")
            fixup = true;
        else if (arg == "--devblocks")
            devblocks = true;
        else if (arg == "--search" && i + 1 < arguments.size())
            search_paths.emplace_back(arguments[++i]);
        else if (arg == "--limit" && i + 1 < arguments.size())
        {
            auto const value = std::stoull(arguments[++i]);
            limit = static_cast<std::size_t>(value);
        }
        else if (arg == "--tick-rate" && i + 1 < arguments.size())
            tick_rate = std::stod(arguments[++i]);
        else if (arg == "--frames" && i + 1 < arguments.size())
            frames = static_cast<std::size_t>(std::stoull(arguments[++i]));
        else if (arg == "--time" && i + 1 < arguments.size())
            tick_time = std::stod(arguments[++i]);
        else if (arg == "--notify" && i + 2 < arguments.size())
        {
            notify_event = arguments[++i];
            notify_target = arguments[++i];
            while (i + 1 < arguments.size() && !arguments[i + 1].starts_with("--"))
                notify_args.push_back(arguments[++i]);
        }
        else if (!script_path)
            script_path = arg;
        else if (!function_name)
            function_name = arg;
        else
            call_args.push_back(arg);
    }

    if (!script_path)
    {
        print_usage();
        return 2;
    }

    auto config = VMConfig{};
    config.instance = client ? Instance::client : Instance::server;
    config.t6_fixup = fixup;
    config.execute_devblocks = devblocks;
    if (limit)
        config.instruction_limit = *limit;
    if (tick_rate)
    {
        config.execution_mode = ExecutionMode::tick_based;
        config.tick_rate = *tick_rate;
    }

    auto host = std::make_shared<DefaultHost>();
    config.host = host;

    auto vm = VM{ config };
    vm.add_search_path(std::filesystem::current_path());
    vm.add_search_path(script_path->parent_path().empty() ? std::filesystem::path{ "." } : script_path->parent_path());
    for (auto const& path : search_paths)
        vm.add_search_path(path);

    auto register_print = [&](std::string const& space, std::string const& name, bool newline) {
        vm.natives().register_function(space, name, [newline](NativeContext& context) {
            auto const format = context.argument_count() == 0 ? std::string{} : context.argument(0).to_string();
            auto arguments = std::vector<Value>{};

            for (std::size_t i = 1; i < context.argument_count(); i++)
                arguments.push_back(context.argument(i));

            std::cout << format_message(format, arguments);
            if (newline)
                std::cout << '\n';
        });
    };

    register_print("", "print", false);
    register_print("", "println", true);
    register_print("", "iprintln", true);
    register_print("", "iprintlnbold", true);

    vm.natives().register_function("", "gettime", [](NativeContext& context) {
        context.set_return_value(Value::floating(static_cast<float>(context.vm().time())));
    });

    try
    {
        auto info = vm.load_module_file(*script_path, script_path->stem().string());

        if (show_info || show_list)
            print_module(info, show_list);
        if (show_disasm)
            std::cout << vm.disassembly(info.name);

        if (show_info || show_list || show_disasm)
            return 0;

        if (!function_name)
            function_name = "main";

        try
        {
            vm.load_dependencies(info.name);
        }
        catch (gsc::Error const& e)
        {
            std::cerr << "warning: " << e.what() << '\n';
        }

        auto args = std::vector<Value>{};
        for (auto const& arg : call_args)
            args.push_back(parse_value(arg));

        if (notify_event)
        {
            auto target = parse_value(*notify_target);
            auto values = std::vector<Value>{};
            for (auto const& arg : notify_args)
                values.push_back(parse_value(arg));
            vm.notify(target, *notify_event, values);
        }

        auto result = vm.call(info.name, *function_name, args);
        std::cout << std::format("{}: {}\n", *function_name, result.to_display_string());

        for (std::size_t i = 0; i < frames; i++)
            vm.tick(tick_time != 0.0 ? tick_time : config.frame_time);

        if (vm.has_live_threads())
            vm.run_until_complete();

        return 0;
    }
    catch (RuntimeError const& e)
    {
        std::cerr << "runtime error: " << e.what() << '\n';
        return 3;
    }
    catch (std::exception const& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
