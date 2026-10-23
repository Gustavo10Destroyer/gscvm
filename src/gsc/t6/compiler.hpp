// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gsc/t6/vm.hpp"

namespace gsc::t6::detail
{

struct CompileOptions
{
    Instance instance{ Instance::server };
    bool dev_blocks{};
    std::vector<std::filesystem::path> search_paths;
    // Optional in-memory include resolver used before filesystem lookup.
    std::function<std::vector<std::uint8_t>(std::string const&)> virtual_loader;
};

auto is_compiled(std::span<std::uint8_t const> data) -> bool;
auto compile_source(std::string const& name, std::string_view source, CompileOptions const& options) -> std::vector<std::uint8_t>;

} // namespace gsc::t6::detail
