// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gsc::wasm
{

auto js_bridge_available(std::string_view name) -> bool;
auto js_native_call(int id, std::string_view meta_json, std::string_view args_json) -> std::string;
auto js_host_call(int id, std::string_view operation, std::string_view payload_json) -> std::string;
auto js_host_object_call(int id, std::string_view operation, std::string_view payload_json) -> std::string;

} // namespace gsc::wasm
