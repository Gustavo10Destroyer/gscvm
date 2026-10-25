// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gsc::wasm
{

struct Json
{
    enum class Kind
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Kind kind{ Kind::null };
    bool boolean{};
    double number{};
    std::string string;
    Array array;
    Object object;

    Json() = default;
    Json(std::nullptr_t);
    Json(bool value);
    Json(double value);
    Json(int value);
    Json(std::string value);
    Json(char const* value);
    Json(Array value);
    Json(Object value);

    static auto null() -> Json;
    static auto make_array() -> Json;
    static auto make_object() -> Json;

    auto is_null() const -> bool;
    auto is_boolean() const -> bool;
    auto is_number() const -> bool;
    auto is_string() const -> bool;
    auto is_array() const -> bool;
    auto is_object() const -> bool;

    auto find(std::string_view key) const -> Json const*;
    auto at(std::string_view key) const -> Json const&;
    auto append(Json value) -> Json&;
    auto set(std::string key, Json value) -> Json&;

    auto as_bool() const -> bool;
    auto as_number() const -> double;
    auto as_string() const -> std::string const&;
};

auto parse_json(std::string_view text) -> Json;
auto stringify_json(Json const& value) -> std::string;
auto json_quote(std::string_view text) -> std::string;

} // namespace gsc::wasm
