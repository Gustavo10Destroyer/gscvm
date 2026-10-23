// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/host.hpp"

#include <cstdio>

namespace gsc::t6
{

auto log_level_name(LogLevel level) -> std::string_view
{
    switch (level)
    {
        case LogLevel::trace: return "trace";
        case LogLevel::debug: return "debug";
        case LogLevel::info: return "info";
        case LogLevel::warning: return "warning";
        case LogLevel::error: return "error";
    }

    return "info";
}

auto Host::log(LogLevel, std::string_view) -> void {}
auto Host::dvar(std::string_view) const -> Value { return Value::undefined(); }
auto Host::dvar_int(std::string_view) const -> std::int32_t { return 0; }
auto Host::dvar_float(std::string_view) const -> float { return 0.0f; }
auto Host::dvar_vector(std::string_view) const -> Vector3 { return {}; }
auto Host::load_script(std::string_view) const -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; }
auto Host::level_object() const -> ObjectPtr { return nullptr; }
auto Host::game_object() const -> ObjectPtr { return nullptr; }
auto Host::world_object() const -> ObjectPtr { return nullptr; }
auto Host::classes_object() const -> ObjectPtr { return nullptr; }
auto Host::anim_object() const -> ObjectPtr { return nullptr; }
auto Host::create_object(std::string_view class_name) -> ObjectPtr { return std::make_shared<ScriptObject>(std::string(class_name)); }

auto DefaultHost::log(LogLevel level, std::string_view message) -> void
{
    std::fprintf(stderr, "[%s] %.*s\n", std::string(log_level_name(level)).c_str(), static_cast<int>(message.size()), message.data());
}

auto DefaultHost::dvar(std::string_view name) const -> Value
{
    auto const it = dvars_.find(name);
    return it == dvars_.end() ? Value::undefined() : it->second;
}

auto DefaultHost::dvar_int(std::string_view name) const -> std::int32_t
{
    return dvar(name).to_integer();
}

auto DefaultHost::dvar_float(std::string_view name) const -> float
{
    return dvar(name).to_floating();
}

auto DefaultHost::dvar_vector(std::string_view name) const -> Vector3
{
    auto const value = dvar(name);
    return value.is_vector() ? value.as_vector() : Vector3{};
}

auto DefaultHost::create_object(std::string_view class_name) -> ObjectPtr
{
    return std::make_shared<ScriptObject>(std::string(class_name));
}

auto DefaultHost::set_dvar(std::string name, Value value) -> void
{
    dvars_.insert_or_assign(std::move(name), std::move(value));
}

} // namespace gsc::t6
