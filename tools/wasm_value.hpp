// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gsc/t6/value.hpp"
#include "wasm_json.hpp"

namespace gsc::wasm
{

auto encode_value(gsc::t6::Value const& value) -> Json;
auto decode_value(Json const& value) -> gsc::t6::Value;
auto encode_values(std::span<gsc::t6::Value const> values) -> Json;
auto decode_values(Json const& values) -> std::vector<gsc::t6::Value>;

auto value_object(gsc::t6::Value const& value) -> Json;
auto value_json(gsc::t6::Value const& value) -> std::string;
auto value_from_json(std::string_view text) -> gsc::t6::Value;
auto values_from_json(std::string_view text) -> std::vector<gsc::t6::Value>;

} // namespace gsc::wasm
