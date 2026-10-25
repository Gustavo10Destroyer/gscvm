// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "wasm_bridge.hpp"

#include <format>
#include <string>

#include <emscripten/val.h>

#include "gsc/error.hpp"

namespace gsc::wasm
{

namespace
{

auto bridge(std::string_view name) -> emscripten::val
{
    auto value = emscripten::val::global(std::string{ name }.c_str());
    if (value.isUndefined() || value.isNull())
        throw gsc::Error(std::format("JavaScript bridge '{}' is not installed", name));
    return value;
}

auto invoke(std::string_view name, emscripten::val const& first, std::string_view operation, std::string_view payload) -> std::string
{
    auto function = bridge(name);
    auto result = function(first, std::string{ operation }, std::string{ payload });
    if (result.isUndefined() || result.isNull())
        return {};
    if (!result.isString())
        throw gsc::Error(std::format("JavaScript bridge '{}' must return a JSON string", name));
    return result.as<std::string>();
}

} // namespace

auto js_bridge_available(std::string_view name) -> bool
{
    auto value = emscripten::val::global(std::string{ name }.c_str());
    return !value.isUndefined() && !value.isNull();
}

auto js_native_call(int id, std::string_view meta_json, std::string_view args_json) -> std::string
{
    auto function = bridge("__gscvmNativeCall");
    auto result = function(id, std::string{ meta_json }, std::string{ args_json });
    if (result.isUndefined() || result.isNull())
        return {};
    if (!result.isString())
        throw gsc::Error("__gscvmNativeCall must return a JSON string");
    return result.as<std::string>();
}

auto js_host_call(int id, std::string_view operation, std::string_view payload_json) -> std::string
{
    return invoke("__gscvmHostCall", emscripten::val{ id }, operation, payload_json);
}

auto js_host_object_call(int id, std::string_view operation, std::string_view payload_json) -> std::string
{
    return invoke("__gscvmHostObjectCall", emscripten::val{ id }, operation, payload_json);
}

} // namespace gsc::wasm
