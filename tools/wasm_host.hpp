// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "gsc/t6/host.hpp"
#include "gsc/t6/value.hpp"
#include "wasm_json.hpp"

namespace gsc::wasm
{

class WasmHost final : public gsc::t6::DefaultHost
{
public:
    explicit WasmHost(int host_id = 0);

    auto host_id() const -> int;
    auto set_host_id(int id) -> void;
    auto has_js_host() const -> bool;

    auto append_output(std::string_view text) -> void;
    auto take_output() -> std::string;
    auto clear_output() -> void;

    auto log(gsc::t6::LogLevel level, std::string_view message) -> void override;

    auto dvar(std::string_view name) const -> gsc::t6::Value override;
    auto dvar_int(std::string_view name) const -> std::int32_t override;
    auto dvar_float(std::string_view name) const -> float override;
    auto dvar_vector(std::string_view name) const -> gsc::t6::Vector3 override;

    auto load_script(std::string_view name) const -> std::optional<std::vector<std::uint8_t>> override;

    auto level_object() const -> gsc::t6::ObjectPtr override;
    auto game_object() const -> gsc::t6::ObjectPtr override;
    auto world_object() const -> gsc::t6::ObjectPtr override;
    auto classes_object() const -> gsc::t6::ObjectPtr override;
    auto anim_object() const -> gsc::t6::ObjectPtr override;

    auto create_object(std::string_view class_name) -> gsc::t6::ObjectPtr override;

private:
    auto call(std::string_view operation, Json payload) const -> Json;
    auto maybe_call(std::string_view operation, Json payload) const -> std::optional<Json>;
    auto object_from(Json const& value) const -> gsc::t6::ObjectPtr;

    int host_id_{};
    mutable std::string output_;
};

auto make_host_object(Json const& value) -> gsc::t6::ObjectPtr;

} // namespace gsc::wasm
