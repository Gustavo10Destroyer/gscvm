// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "wasm_value.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

#include "gsc/error.hpp"

namespace gsc::wasm
{

using gsc::t6::AnimationRef;
using gsc::t6::AnimTreeRef;
using gsc::t6::Array;
using gsc::t6::FunctionRef;
using gsc::t6::Object;
using gsc::t6::ScriptObject;
using gsc::t6::ThreadRef;
using gsc::t6::Value;
using gsc::t6::Vector3;

namespace
{

auto tag(std::string_view name) -> Json
{
    auto out = Json::make_object();
    out.set("$gsc", Json{ std::string{ name } });
    return out;
}

auto is_tag(Json const& value, std::string_view name) -> bool
{
    if (!value.is_object())
        return false;
    auto const* found = value.find("$gsc");
    return found && found->is_string() && found->string == name;
}

auto trim_number(double value) -> bool
{
    return std::isfinite(value) && std::trunc(value) == value
        && value >= static_cast<double>(std::numeric_limits<std::int32_t>::min())
        && value <= static_cast<double>(std::numeric_limits<std::int32_t>::max());
}

auto number_to_value(double number) -> Value
{
    if (trim_number(number))
        return Value::integer(static_cast<std::int32_t>(number));
    return Value::floating(static_cast<float>(number));
}

auto decode_impl(Json const& value, std::unordered_map<int, Value>& references, std::unordered_map<void const*, int>& encoded) -> Value;

auto object_from_json(Json const& value, std::unordered_map<int, Value>& references, std::unordered_map<void const*, int>& encoded) -> Value
{
    auto const* class_value = value.find("class");
    auto const class_name = class_value && class_value->is_string() ? class_value->string : std::string{ "object" };
    auto object = std::make_shared<ScriptObject>(class_name);

    auto result = Value::object(object);
    if (auto const* id_value = value.find("id"); id_value && id_value->is_number())
        references[static_cast<int>(id_value->number)] = result;

    auto const* entries = value.find("entries");
    if (entries && entries->is_array())
    {
        for (auto const& entry : entries->array)
        {
            if (!entry.is_array() || entry.array.size() != 2)
                throw gsc::Error("object entry must be a [key, value] pair");
            object->set_index(decode_impl(entry.array[0], references, encoded), decode_impl(entry.array[1], references, encoded));
        }
    }
    else
    {
        if (value.is_object())
        {
            for (auto const& [key, entry] : value.object)
            {
                if (key == "$gsc" || key == "class" || key == "id" || key == "entries")
                    continue;
                object->set_field(key, decode_impl(entry, references, encoded));
            }
        }
    }

    return result;
}

auto array_from_json(Json const& value, std::unordered_map<int, Value>& references, std::unordered_map<void const*, int>& encoded) -> Value
{
    auto array = std::make_shared<Array>();
    auto result = Value::array(array);
    if (auto const* id_value = value.find("id"); id_value && id_value->is_number())
        references[static_cast<int>(id_value->number)] = result;

    auto const* entries = value.find("entries");
    if (entries && entries->is_array())
    {
        for (auto const& entry : entries->array)
        {
            if (!entry.is_array() || entry.array.size() != 2)
                throw gsc::Error("array entry must be a [key, value] pair");
            array->set(decode_impl(entry.array[0], references, encoded), decode_impl(entry.array[1], references, encoded));
        }
    }
    return result;
}

auto decode_impl(Json const& value, std::unordered_map<int, Value>& references, std::unordered_map<void const*, int>& encoded) -> Value
{
    (void)encoded;
    if (value.is_null())
        return Value::undefined();
    if (value.is_boolean())
        return Value::boolean(value.boolean);
    if (value.is_number())
        return number_to_value(value.number);
    if (value.is_string())
        return Value::string(value.string);
    if (value.is_array())
    {
        auto array = std::make_shared<Array>();
        auto result = Value::array(array);
        for (std::size_t i = 0; i < value.array.size(); i++)
            array->set(Value::integer(static_cast<std::int32_t>(i)), decode_impl(value.array[i], references, encoded));
        return result;
    }
    if (!value.is_object())
        throw gsc::Error("unsupported JSON value");

    auto const* tag_value = value.find("$gsc");
    if (!tag_value || !tag_value->is_string())
    {
        auto object = std::make_shared<ScriptObject>("object");
        auto result = Value::object(object);
        auto const* entries = value.find("entries");
        if (entries && entries->is_array())
        {
            for (auto const& entry : entries->array)
            {
                if (!entry.is_array() || entry.array.size() != 2)
                    throw gsc::Error("object entry must be a [key, value] pair");
                object->set_index(decode_impl(entry.array[0], references, encoded), decode_impl(entry.array[1], references, encoded));
            }
        }
        else
        {
            for (auto const& [key, entry] : value.object)
            {
                if (key == "$gsc" || key == "class" || key == "id" || key == "entries")
                    continue;
                object->set_field(key, decode_impl(entry, references, encoded));
            }
        }
        return result;
    }

    auto const& name = tag_value->string;
    if (name == "undefined")
        return Value::undefined();
    if (name == "ref")
    {
        auto const id = static_cast<int>(value.at("id").as_number());
        auto const found = references.find(id);
        if (found == references.end())
            throw gsc::Error(std::format("unknown value reference {}", id));
        return found->second;
    }
    if (name == "integer")
        return Value::integer(static_cast<std::int32_t>(value.at("value").as_number()));
    if (name == "float")
    {
        auto const& raw = value.at("value");
        if (raw.is_string())
        {
            if (raw.string == "nan") return Value::floating(std::numeric_limits<float>::quiet_NaN());
            if (raw.string == "inf") return Value::floating(std::numeric_limits<float>::infinity());
            if (raw.string == "-inf") return Value::floating(-std::numeric_limits<float>::infinity());
            throw gsc::Error("invalid float value");
        }
        return Value::floating(static_cast<float>(raw.as_number()));
    }
    if (name == "istring")
        return Value::istring(value.at("value").as_string());
    if (name == "hash")
        return Value::hash(value.at("value").as_string());
    if (name == "vector")
    {
        auto const x = static_cast<float>(value.at("x").as_number());
        auto const y = static_cast<float>(value.at("y").as_number());
        auto const z = static_cast<float>(value.at("z").as_number());
        return Value::vector(x, y, z);
    }
    if (name == "array")
        return array_from_json(value, references, encoded);
    if (name == "object")
        return object_from_json(value, references, encoded);
    if (name == "function")
    {
        auto reference = FunctionRef{};
        auto const* kind = value.find("kind");
        auto const kind_name = kind && kind->is_string() ? kind->string : std::string{ "unresolved" };
        reference.kind = kind_name == "script" ? FunctionRef::Kind::script
            : kind_name == "native" ? FunctionRef::Kind::native : FunctionRef::Kind::unresolved;
        if (auto const* field = value.find("module"); field && field->is_string()) reference.module = field->string;
        if (auto const* field = value.find("space"); field && field->is_string()) reference.space = field->string;
        if (auto const* field = value.find("name"); field && field->is_string()) reference.name = field->string;
        if (auto const* field = value.find("params"); field && field->is_number()) reference.params = static_cast<std::uint8_t>(field->number);
        if (auto const* field = value.find("flags"); field && field->is_number()) reference.flags = static_cast<std::uint8_t>(field->number);
        return Value::function(reference);
    }
    if (name == "thread")
        return Value::thread(ThreadRef{ static_cast<std::uint64_t>(value.at("id").as_number()) });
    if (name == "animtree")
        return Value::animtree(value.at("name").as_string());
    if (name == "animation")
        return Value::animation(value.at("tree").as_string(), value.at("name").as_string());

    throw gsc::Error(std::format("unknown value tag '{}'", name));
}

class Encoder
{
public:
    auto encode(Value const& value) -> Json
    {
        switch (value.type())
        {
            case Value::Type::undefined:
                return Json{};
            case Value::Type::boolean:
                return Json{ value.as_boolean() };
            case Value::Type::integer:
                return Json{ static_cast<double>(value.as_integer()) };
            case Value::Type::floating:
            {
                auto const number = value.as_floating();
                if (!std::isfinite(number))
                {
                    auto out = tag("float");
                    out.set("value", Json{ std::isnan(number) ? "nan" : (number < 0.0f ? "-inf" : "inf") });
                    return out;
                }
                if (std::trunc(number) == number)
                {
                    auto out = tag("float");
                    out.set("value", Json{ static_cast<double>(number) });
                    return out;
                }
                return Json{ static_cast<double>(number) };
            }
            case Value::Type::string:
                return Json{ value.as_string() };
            case Value::Type::istring:
            {
                auto out = tag("istring");
                out.set("value", Json{ value.as_string() });
                return out;
            }
            case Value::Type::hash:
            {
                auto out = tag("hash");
                out.set("value", Json{ value.as_string() });
                return out;
            }
            case Value::Type::vector:
            {
                auto const& vector = value.as_vector();
                auto out = tag("vector");
                out.set("x", Json{ static_cast<double>(vector.x) });
                out.set("y", Json{ static_cast<double>(vector.y) });
                out.set("z", Json{ static_cast<double>(vector.z) });
                return out;
            }
            case Value::Type::array:
                return encode_array(value.as_array());
            case Value::Type::object:
                return encode_object(value.as_object());
            case Value::Type::function:
            {
                auto const& function = value.as_function();
                auto out = tag("function");
                out.set("kind", Json{ function.kind == FunctionRef::Kind::script ? "script" : function.kind == FunctionRef::Kind::native ? "native" : "unresolved" });
                out.set("module", Json{ function.module });
                out.set("space", Json{ function.space });
                out.set("name", Json{ function.name });
                out.set("params", Json{ static_cast<double>(function.params) });
                out.set("flags", Json{ static_cast<double>(function.flags) });
                return out;
            }
            case Value::Type::thread:
            {
                auto out = tag("thread");
                out.set("id", Json{ static_cast<double>(value.as_thread().id) });
                return out;
            }
            case Value::Type::animtree:
            {
                auto out = tag("animtree");
                out.set("name", Json{ value.as_animtree().name });
                return out;
            }
            case Value::Type::animation:
            {
                auto const& animation = value.as_animation();
                auto out = tag("animation");
                out.set("tree", Json{ animation.tree });
                out.set("name", Json{ animation.name });
                return out;
            }
        }
        return Json{};
    }

private:
    auto encode_array(Array const& array) -> Json
    {
        if (auto const it = seen_.find(&array); it != seen_.end())
        {
            auto out = tag("ref");
            out.set("id", Json{ it->second });
            return out;
        }

        auto const id = next_id_++;
        seen_[&array] = id;

        auto out = tag("array");
        out.set("id", Json{ id });
        auto entries = Json::make_array();
        for (auto const& key : array.keys())
        {
            auto pair = Json::make_array();
            pair.append(encode(key));
            pair.append(encode(array.get(key)));
            entries.append(std::move(pair));
        }
        out.set("entries", std::move(entries));
        return out;
    }

    auto encode_object(Object const& object) -> Json
    {
        if (auto const it = seen_.find(&object); it != seen_.end())
        {
            auto out = tag("ref");
            out.set("id", Json{ it->second });
            return out;
        }

        auto const id = next_id_++;
        seen_[&object] = id;

        auto out = tag("object");
        out.set("id", Json{ id });
        out.set("class", Json{ std::string{ object.class_name() } });
        auto entries = Json::make_array();
        for (auto const& key : object.keys())
        {
            auto pair = Json::make_array();
            pair.append(encode(key));
            pair.append(encode(object.get_index(key)));
            entries.append(std::move(pair));
        }
        out.set("entries", std::move(entries));
        return out;
    }

    std::unordered_map<void const*, int> seen_;
    int next_id_{ 1 };
};

} // namespace

auto encode_value(Value const& value) -> Json
{
    return Encoder{}.encode(value);
}

auto decode_value(Json const& value) -> Value
{
    auto references = std::unordered_map<int, Value>{};
    auto encoded = std::unordered_map<void const*, int>{};
    return decode_impl(value, references, encoded);
}

auto encode_values(std::span<Value const> values) -> Json
{
    auto encoder = Encoder{};
    auto out = Json::make_array();
    for (auto const& value : values)
        out.append(encoder.encode(value));
    return out;
}

auto decode_values(Json const& values) -> std::vector<Value>
{
    if (!values.is_array())
        throw gsc::Error("expected a JSON array of values");
    auto references = std::unordered_map<int, Value>{};
    auto encoded = std::unordered_map<void const*, int>{};
    auto out = std::vector<Value>{};
    out.reserve(values.array.size());
    for (auto const& value : values.array)
        out.push_back(decode_impl(value, references, encoded));
    return out;
}

auto value_object(Value const& value) -> Json
{
    auto out = Json::make_object();
    out.set("type", Json{ std::string{ value.type_name() } });
    out.set("display", Json{ value.to_display_string() });
    out.set("value", encode_value(value));
    return out;
}

auto value_json(Value const& value) -> std::string
{
    return stringify_json(value_object(value));
}

auto value_from_json(std::string_view text) -> Value
{
    return decode_value(parse_json(text));
}

auto values_from_json(std::string_view text) -> std::vector<Value>
{
    return decode_values(parse_json(text));
}

} // namespace gsc::wasm
