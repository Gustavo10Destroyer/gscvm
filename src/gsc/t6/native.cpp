// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/native.hpp"

#include <algorithm>
#include <cctype>

namespace gsc::t6
{

namespace
{

auto lower(std::string_view value) -> std::string
{
    auto out = std::string{ value };
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

auto key_of(std::string_view space, std::string_view name) -> std::string
{
    if (space.empty())
        return lower(name);
    return lower(space) + "::" + lower(name);
}

} // namespace

auto NativeRegistry::register_function(std::string_view name, NativeFunction function) -> void
{
    register_function({}, name, std::move(function));
}

auto NativeRegistry::register_function(std::string_view space, std::string_view name, NativeFunction function) -> void
{
    functions_.insert_or_assign(key_of(space, name), std::move(function));
}

auto NativeRegistry::unregister_function(std::string_view name) -> bool
{
    return functions_.erase(lower(name)) != 0;
}

auto NativeRegistry::unregister_function(std::string_view space, std::string_view name) -> bool
{
    return functions_.erase(key_of(space, name)) != 0;
}

auto NativeRegistry::contains(std::string_view name) const -> bool
{
    return find({}, name) != nullptr;
}

auto NativeRegistry::contains(std::string_view space, std::string_view name) const -> bool
{
    return find(space, name) != nullptr;
}

auto NativeRegistry::find(std::string_view space, std::string_view name) const -> NativeFunction const*
{
    if (!space.empty())
    {
        auto const it = functions_.find(key_of(space, name));
        if (it != functions_.end())
            return &it->second;
    }

    auto const it = functions_.find(lower(name));
    return it == functions_.end() ? nullptr : &it->second;
}

auto NativeRegistry::names() const -> std::vector<std::string>
{
    auto out = std::vector<std::string>{};
    out.reserve(functions_.size());
    for (auto const& [name, function] : functions_)
    {
        (void)function;
        out.push_back(name);
    }
    return out;
}

} // namespace gsc::t6
