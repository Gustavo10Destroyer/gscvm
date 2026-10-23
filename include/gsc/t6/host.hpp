// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gsc/t6/value.hpp"

namespace gsc::t6
{

enum class LogLevel : std::uint8_t
{
    trace,
    debug,
    info,
    warning,
    error,
};

// The host supplies everything that is engine-specific. The VM owns scheduling
// and bytecode semantics; it never pretends an engine service exists.
class Host
{
public:
    virtual ~Host() = default;

    virtual auto log(LogLevel level, std::string_view message) -> void;

    // Dvars. Returning undefined means "not present"; the VM converts that to
    // undefined/0/zero-vector according to the opcode.
    virtual auto dvar(std::string_view name) const -> Value;
    virtual auto dvar_int(std::string_view name) const -> std::int32_t;
    virtual auto dvar_float(std::string_view name) const -> float;
    virtual auto dvar_vector(std::string_view name) const -> Vector3;

    // Optional script resolver. The VM also searches its configured paths.
    virtual auto load_script(std::string_view name) const -> std::optional<std::vector<std::uint8_t>>;

    // Engine singletons. A null result makes the VM use a private ScriptObject.
    virtual auto level_object() const -> ObjectPtr;
    virtual auto game_object() const -> ObjectPtr;
    virtual auto world_object() const -> ObjectPtr;
    virtual auto classes_object() const -> ObjectPtr;
    virtual auto anim_object() const -> ObjectPtr;

    // Used by object-creating opcodes and host natives.
    virtual auto create_object(std::string_view class_name) -> ObjectPtr;
};

// Small headless host: logs to stderr, keeps an in-memory dvar table and loads
// scripts from the VM's search paths.
class DefaultHost final : public Host
{
public:
    auto log(LogLevel level, std::string_view message) -> void override;

    auto dvar(std::string_view name) const -> Value override;
    auto dvar_int(std::string_view name) const -> std::int32_t override;
    auto dvar_float(std::string_view name) const -> float override;
    auto dvar_vector(std::string_view name) const -> Vector3 override;

    auto create_object(std::string_view class_name) -> ObjectPtr override;

    auto set_dvar(std::string name, Value value) -> void;

private:
    std::map<std::string, Value, std::less<>> dvars_;
};

auto log_level_name(LogLevel level) -> std::string_view;

} // namespace gsc::t6
