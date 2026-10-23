// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gsc/error.hpp"
#include "gsc/t6/vm.hpp"

namespace xsk::arc
{
enum class opcode : std::uint8_t;
}

namespace gsc::t6::detail
{

struct SwitchEntry
{
    bool is_default{};
    bool is_string{};
    std::int32_t integer{};
    std::string string;
    std::size_t target_offset{};
    std::size_t target_index{};
};

struct Instruction
{
    xsk::arc::opcode opcode{};
    std::string name;
    std::size_t offset{};
    std::size_t size{};
    std::vector<std::string> data;
    std::vector<SwitchEntry> switch_entries;
    std::size_t jump_target{};
    std::size_t switch_target{};
    bool has_switch_table{};
};

struct Function
{
    std::string name;
    std::string space;
    std::uint8_t params{};
    std::uint8_t flags{};
    std::size_t offset{};
    std::size_t size{};
    std::vector<Instruction> instructions;
    std::unordered_map<std::size_t, std::size_t> offset_to_index;

    auto find_index(std::size_t absolute_offset) const -> std::optional<std::size_t>;
    auto find_label_target(std::string_view label) const -> std::optional<std::size_t>;
};

struct Module
{
    std::string name;
    std::vector<std::string> includes;
    std::vector<Function> functions;
    std::vector<std::uint8_t> bytes;
    std::string disassembly;
    mutable std::optional<ModuleInfo> info;

    auto find_function(std::string_view space, std::string_view name) const -> Function const*;
    auto has_function(std::string_view space, std::string_view name) const -> bool;

    std::unordered_map<std::string, std::size_t> by_qualified_name;
    std::unordered_map<std::string, std::size_t> by_name;
};

struct LoadOptions
{
    Instance instance{ Instance::server };
    bool t6_fixup{};
};

auto load_module(std::string name, std::uint8_t const* data, std::size_t size, LoadOptions const& options) -> std::shared_ptr<Module>;

} // namespace gsc::t6::detail
