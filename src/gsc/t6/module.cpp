// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/module.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <stdexcept>

#include "xsk/arc/engine/t6_pc.hpp"
#include "xsk/arc/printer.hpp"

namespace gsc::t6::detail
{

namespace
{

auto lower(std::string_view value) -> std::string
{
    auto out = std::string{ value };
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

auto qualified(std::string_view space, std::string_view name) -> std::string
{
    if (space.empty())
        return lower(name);
    return lower(space) + "::" + lower(name);
}

auto parse_label(std::string_view label) -> std::size_t
{
    if (!label.starts_with("loc_"))
        throw Error(std::format("invalid control-flow label '{}'", label));

    auto value = std::size_t{};
    auto const* first = label.data() + 4;
    auto const* last = label.data() + label.size();
    auto const result = std::from_chars(first, last, value, 16);

    if (result.ec != std::errc{} || result.ptr != last)
        throw Error(std::format("invalid control-flow label '{}'", label));

    return value;
}

auto is_jump(xsk::arc::opcode op) -> bool
{
    using xsk::arc::opcode;

    switch (op)
    {
        case opcode::OP_JumpOnFalse:
        case opcode::OP_JumpOnTrue:
        case opcode::OP_JumpOnFalseExpr:
        case opcode::OP_JumpOnTrueExpr:
        case opcode::OP_Jump:
        case opcode::OP_JumpBack:
        case opcode::OP_DevblockBegin:
            return true;
        default:
            return false;
    }
}

auto resolve_target(Function const& function, std::size_t absolute, std::string_view what, std::size_t source_offset) -> std::size_t
{
    auto const it = function.offset_to_index.find(absolute);
    if (it == function.offset_to_index.end())
        throw Error(std::format("{} target 0x{:X} from 0x{:X} is not an instruction boundary in '{}'", what, absolute, source_offset, function.name));
    return it->second;
}

} // namespace

auto Function::find_index(std::size_t absolute_offset) const -> std::optional<std::size_t>
{
    auto const it = offset_to_index.find(absolute_offset);
    if (it == offset_to_index.end())
        return std::nullopt;
    return it->second;
}

auto Function::find_label_target(std::string_view label) const -> std::optional<std::size_t>
{
    return find_index(parse_label(label));
}

auto Module::find_function(std::string_view space, std::string_view name) const -> Function const*
{
    if (!space.empty())
    {
        auto const it = by_qualified_name.find(qualified(space, name));
        if (it != by_qualified_name.end())
            return &functions[it->second];
    }

    auto const it = by_name.find(lower(name));
    if (it == by_name.end())
        return nullptr;
    return &functions[it->second];
}

auto Module::has_function(std::string_view space, std::string_view name) const -> bool
{
    return find_function(space, name) != nullptr;
}

auto load_module(std::string requested_name, std::uint8_t const* data, std::size_t size, LoadOptions const& options) -> std::shared_ptr<Module>
{
    (void)options.instance;
    (void)options.t6_fixup;

    if (data == nullptr || size == 0)
        throw Error("cannot load an empty T6 bytecode buffer");

    try
    {
        auto context = xsk::arc::t6::pc::context{ options.instance == Instance::client ? xsk::arc::instance::client : xsk::arc::instance::server };
        context.init(xsk::arc::build::prod, [](std::string const&) { return std::vector<std::uint8_t>{}; });
        context.fixup(options.t6_fixup);

        auto assembly = context.disassembler().disassemble(data, size);
        auto module = std::make_shared<Module>();
        module->name = assembly->name.empty() ? std::move(requested_name) : assembly->name;
        module->includes = assembly->includes;
        module->bytes.assign(data, data + size);
        auto const text = context.printer().print(*assembly);
        module->disassembly.assign(reinterpret_cast<char const*>(text.data()), text.size());

        module->functions.reserve(assembly->functions.size());

        for (auto const& source : assembly->functions)
        {
            auto function = Function{};
            function.name = source->name;
            function.space = source->space;
            function.params = source->params;
            function.flags = source->flags;
            function.offset = source->index;
            function.size = source->size;
            function.instructions.reserve(source->instructions.size());

            for (auto const& src : source->instructions)
            {
                auto instruction = Instruction{};
                instruction.opcode = src->opcode;
                instruction.name = context.opcode_name(src->opcode);
                instruction.offset = src->index;
                instruction.size = src->size;
                instruction.data = src->data;
                function.instructions.push_back(std::move(instruction));
            }

            for (std::size_t i = 0; i < function.instructions.size(); i++)
                function.offset_to_index.emplace(function.instructions[i].offset, i);

            module->functions.push_back(std::move(function));
        }

        // Resolve all control flow now, so execution never has to perform a
        // partial or unsafe jump lookup.
        for (auto& function : module->functions)
        {
            for (auto& instruction : function.instructions)
            {
                if (is_jump(instruction.opcode) && !instruction.data.empty())
                {
                    auto const target = parse_label(instruction.data[0]);
                    instruction.jump_target = resolve_target(function, target, "jump", instruction.offset);
                }

                if (instruction.opcode != xsk::arc::opcode::OP_Switch)
                    continue;

                if (instruction.data.empty())
                    throw Error(std::format("switch without a table at 0x{:X} in '{}'", instruction.offset, function.name));

                auto const table_offset = parse_label(instruction.data[0]);
                auto const table_it = function.offset_to_index.find(table_offset);

                if (table_it == function.offset_to_index.end())
                    throw Error(std::format("switch table 0x{:X} from 0x{:X} is not an instruction in '{}'", table_offset, instruction.offset, function.name));

                auto const& table = function.instructions[table_it->second];
                if (table.opcode != xsk::arc::opcode::OP_EndSwitch)
                    throw Error(std::format("switch at 0x{:X} does not point at an OP_EndSwitch in '{}'", instruction.offset, function.name));

                instruction.has_switch_table = true;
                instruction.switch_target = table_it->second;

                if (table.data.empty())
                    throw Error(std::format("malformed switch table at 0x{:X} in '{}'", table.offset, function.name));

                auto count = std::size_t{};
                {
                    auto const result = std::from_chars(table.data[0].data(), table.data[0].data() + table.data[0].size(), count);
                    if (result.ec != std::errc{})
                        throw Error(std::format("malformed switch count at 0x{:X} in '{}'", table.offset, function.name));
                }

                for (std::size_t i = 0; i < count; i++)
                {
                    auto const base = 1 + (4 * i);
                    if (base >= table.data.size())
                        throw Error(std::format("truncated switch table at 0x{:X} in '{}'", table.offset, function.name));

                    auto entry = SwitchEntry{};

                    if (table.data[base] == "default")
                    {
                        entry.is_default = true;
                        if (base + 1 >= table.data.size())
                            throw Error(std::format("truncated switch default at 0x{:X}", table.offset));
                        entry.target_offset = parse_label(table.data[base + 1]);
                    }
                    else if (table.data[base] == "case")
                    {
                        if (base + 3 >= table.data.size())
                            throw Error(std::format("truncated switch case at 0x{:X}", table.offset));

                        auto const type = std::stoi(table.data[base + 1]);
                        if (type == 2)
                        {
                            entry.is_string = true;
                            entry.string = table.data[base + 2];
                        }
                        else
                        {
                            entry.integer = std::stoi(table.data[base + 2]);
                        }

                        entry.target_offset = parse_label(table.data[base + 3]);
                    }
                    else
                    {
                        throw Error(std::format("invalid switch entry '{}' at 0x{:X}", table.data[base], table.offset));
                    }

                    entry.target_index = resolve_target(function, entry.target_offset, "switch case", table.offset);
                    instruction.switch_entries.push_back(std::move(entry));
                }
            }
        }

        for (std::size_t i = 0; i < module->functions.size(); i++)
        {
            auto const& function = module->functions[i];
            module->by_name.try_emplace(lower(function.name), i);
            if (!function.space.empty())
                module->by_qualified_name.try_emplace(qualified(function.space, function.name), i);
        }

        return module;
    }
    catch (Error const&)
    {
        throw;
    }
    catch (std::exception const& e)
    {
        throw Error(std::format("failed to load T6 module '{}': {}", requested_name, e.what()));
    }
}

} // namespace gsc::t6::detail
