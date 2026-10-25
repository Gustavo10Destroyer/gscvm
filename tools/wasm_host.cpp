// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "wasm_host.hpp"

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gsc/error.hpp"
#include "wasm_bridge.hpp"
#include "wasm_value.hpp"

namespace gsc::wasm
{

using gsc::t6::Object;
using gsc::t6::ObjectPtr;
using gsc::t6::ScriptObject;
using gsc::t6::Value;
using gsc::t6::Vector3;

namespace
{

auto is_error(Json const& value) -> std::string
{
    if (!value.is_object())
        return {};
    auto const* error = value.find("$gscError");
    return error && error->is_string() ? error->string : std::string{};
}

auto checked(Json value) -> Json
{
    if (auto const error = is_error(value); !error.empty())
        throw gsc::Error(error);
    return value;
}

auto object_id(Json const& value) -> int
{
    if (!value.is_object())
        return 0;
    auto const* tag_value = value.find("$gsc");
    if (!tag_value || !tag_value->is_string() || tag_value->string != "hostobject")
        return 0;
    auto const* id = value.find("id");
    return id && id->is_number() ? static_cast<int>(id->number) : 0;
}

class JsHostObject final : public Object
{
public:
    JsHostObject(int id, std::string class_name) : id_(id), class_name_(std::move(class_name)) {}

    auto class_name() const -> std::string_view override { return class_name_; }
    auto debug_name() const -> std::string override { return std::format("{}#{}", class_name_, id_); }

    auto to_string() const -> std::string override
    {
        try
        {
            auto result = call("toString", Json::make_object());
            if (result.is_string())
                return result.string;
        }
        catch (...)
        {
        }
        return debug_name();
    }

    auto get_field(std::string_view name) const -> Value override
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        return decode_value(call("get", std::move(payload)));
    }

    auto set_field(std::string_view name, Value value) -> void override
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        payload.set("value", encode_value(value));
        (void)call("set", std::move(payload));
    }

    auto clear_field(std::string_view name) -> bool override
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        return call("clear", std::move(payload)).as_bool();
    }

    auto has_field(std::string_view name) const -> bool override
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        return call("has", std::move(payload)).as_bool();
    }

    auto get_index(Value const& key) const -> Value override
    {
        auto payload = Json::make_object();
        payload.set("key", encode_value(key));
        return decode_value(call("getIndex", std::move(payload)));
    }

    auto set_index(Value const& key, Value value) -> void override
    {
        auto payload = Json::make_object();
        payload.set("key", encode_value(key));
        payload.set("value", encode_value(value));
        (void)call("setIndex", std::move(payload));
    }

    auto clear_index(Value const& key) -> bool override
    {
        auto payload = Json::make_object();
        payload.set("key", encode_value(key));
        return call("clearIndex", std::move(payload)).as_bool();
    }

    auto size() const -> std::size_t override
    {
        return static_cast<std::size_t>(call("size", Json::make_object()).as_number());
    }

    auto first_key() const -> Value override
    {
        auto const keys = this->keys();
        return keys.empty() ? Value::undefined() : keys.front();
    }

    auto next_key(Value const& key) const -> Value override
    {
        auto const keys = this->keys();
        for (std::size_t i = 0; i < keys.size(); i++)
        {
            if (keys[i].equals(key, true) && i + 1 < keys.size())
                return keys[i + 1];
        }
        return Value::undefined();
    }

    auto keys() const -> std::vector<Value> override
    {
        auto result = call("keys", Json::make_object());
        if (!result.is_array())
            throw gsc::Error("host object keys must be an array");
        auto out = std::vector<Value>{};
        out.reserve(result.array.size());
        for (auto const& key : result.array)
            out.push_back(decode_value(key));
        return out;
    }

    auto same_as(Object const& other) const -> bool override
    {
        auto const* object = dynamic_cast<JsHostObject const*>(&other);
        return object && object->id_ == id_;
    }

private:
    auto call(std::string_view operation, Json payload) const -> Json
    {
        auto result = js_host_object_call(id_, operation, stringify_json(payload));
        auto value = result.empty() ? Json{} : parse_json(result);
        return checked(std::move(value));
    }

    int id_{};
    std::string class_name_;
};

} // namespace

auto make_host_object(Json const& value) -> ObjectPtr
{
    if (auto const id = object_id(value); id != 0)
    {
        auto class_name = std::string{ "object" };
        if (auto const* name = value.find("class"); name && name->is_string())
            class_name = name->string;
        return std::make_shared<JsHostObject>(id, std::move(class_name));
    }
    return nullptr;
}

WasmHost::WasmHost(int host_id) : host_id_(host_id) {}

auto WasmHost::host_id() const -> int { return host_id_; }
auto WasmHost::set_host_id(int id) -> void { host_id_ = id; }

auto WasmHost::has_js_host() const -> bool
{
    return host_id_ != 0 && js_bridge_available("__gscvmHostCall");
}

auto WasmHost::append_output(std::string_view text) -> void
{
    output_.append(text);
}

auto WasmHost::take_output() -> std::string
{
    auto result = std::move(output_);
    output_.clear();
    return result;
}

auto WasmHost::clear_output() -> void
{
    output_.clear();
}

auto WasmHost::call(std::string_view operation, Json payload) const -> Json
{
    if (!has_js_host())
        throw gsc::Error("no JavaScript host is configured");
    auto result = js_host_call(host_id_, operation, stringify_json(payload));
    auto value = result.empty() ? Json{} : parse_json(result);
    return checked(std::move(value));
}

auto WasmHost::maybe_call(std::string_view operation, Json payload) const -> std::optional<Json>
{
    if (!has_js_host())
        return std::nullopt;
    try
    {
        auto value = call(operation, std::move(payload));
        if (value.is_null())
            return std::nullopt;
        return value;
    }
    catch (gsc::Error const& error)
    {
        if (std::string_view{ error.what() } == "JavaScript host method is not implemented")
            return std::nullopt;
        throw;
    }
}

auto WasmHost::log(gsc::t6::LogLevel level, std::string_view message) -> void
{
    output_ += "[";
    output_ += gsc::t6::log_level_name(level);
    output_ += "] ";
    output_ += message;
    output_ += "\n";

    if (has_js_host())
    {
        try
        {
            auto payload = Json::make_object();
            payload.set("level", Json{ std::string{ gsc::t6::log_level_name(level) } });
            payload.set("message", Json{ std::string{ message } });
            (void)call("log", std::move(payload));
        }
        catch (...)
        {
        }
    }
}

auto WasmHost::dvar(std::string_view name) const -> Value
{
    if (has_js_host())
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        auto value = decode_value(call("dvar", std::move(payload)));
        if (!value.is_undefined())
            return value;
    }
    return DefaultHost::dvar(name);
}

auto WasmHost::dvar_int(std::string_view name) const -> std::int32_t
{
    if (has_js_host())
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        if (auto value = maybe_call("dvarInt", std::move(payload)))
            return static_cast<std::int32_t>(value->as_number());
        return dvar(name).to_integer();
    }
    return DefaultHost::dvar_int(name);
}

auto WasmHost::dvar_float(std::string_view name) const -> float
{
    if (has_js_host())
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        if (auto value = maybe_call("dvarFloat", std::move(payload)))
            return static_cast<float>(value->as_number());
        return dvar(name).to_floating();
    }
    return DefaultHost::dvar_float(name);
}

auto WasmHost::dvar_vector(std::string_view name) const -> Vector3
{
    if (has_js_host())
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        if (auto value = maybe_call("dvarVector", std::move(payload)))
        {
            if (value->is_array() && value->array.size() == 3)
                return Vector3{
                    static_cast<float>(value->array[0].as_number()),
                    static_cast<float>(value->array[1].as_number()),
                    static_cast<float>(value->array[2].as_number()),
                };
            if (value->is_object())
                return Vector3{
                    static_cast<float>(value->at("x").as_number()),
                    static_cast<float>(value->at("y").as_number()),
                    static_cast<float>(value->at("z").as_number()),
                };
        }
        auto fallback = dvar(name);
        return fallback.is_vector() ? fallback.as_vector() : Vector3{};
    }
    return DefaultHost::dvar_vector(name);
}

auto WasmHost::load_script(std::string_view name) const -> std::optional<std::vector<std::uint8_t>>
{
    if (has_js_host())
    {
        auto payload = Json::make_object();
        payload.set("name", Json{ std::string{ name } });
        auto result = call("loadScript", std::move(payload));
        if (result.is_null())
            return std::nullopt;
        auto out = std::vector<std::uint8_t>{};
        if (result.is_array())
        {
            out.reserve(result.array.size());
            for (auto const& byte : result.array)
                out.push_back(static_cast<std::uint8_t>(byte.as_number()));
            return out;
        }
        if (result.is_string())
        {
            out.assign(result.string.begin(), result.string.end());
            return out;
        }
        throw gsc::Error("host loadScript must return an array of bytes, a string, or null");
    }
    return DefaultHost::load_script(name);
}

auto WasmHost::object_from(Json const& value) const -> ObjectPtr
{
    if (auto object = make_host_object(value))
        return object;
    auto decoded = decode_value(value);
    if (decoded.is_object())
        return decoded.as_object_ptr();
    return nullptr;
}

auto WasmHost::level_object() const -> ObjectPtr
{
    if (!has_js_host())
        return DefaultHost::level_object();
    try
    {
        if (auto value = maybe_call("level", Json::make_object()))
        {
            if (auto object = object_from(*value))
                return object;
        }
    }
    catch (gsc::Error const& error)
    {
        if (std::string_view{ error.what() } != "JavaScript host method is not implemented")
            throw;
    }
    return DefaultHost::level_object();
}

auto WasmHost::game_object() const -> ObjectPtr
{
    if (!has_js_host())
        return DefaultHost::game_object();
    if (auto value = maybe_call("game", Json::make_object()))
    {
        if (auto object = object_from(*value))
            return object;
    }
    return DefaultHost::game_object();
}

auto WasmHost::world_object() const -> ObjectPtr
{
    if (!has_js_host())
        return DefaultHost::world_object();
    if (auto value = maybe_call("world", Json::make_object()))
    {
        if (auto object = object_from(*value))
            return object;
    }
    return DefaultHost::world_object();
}

auto WasmHost::classes_object() const -> ObjectPtr
{
    if (!has_js_host())
        return DefaultHost::classes_object();
    if (auto value = maybe_call("classes", Json::make_object()))
    {
        if (auto object = object_from(*value))
            return object;
    }
    return DefaultHost::classes_object();
}

auto WasmHost::anim_object() const -> ObjectPtr
{
    if (!has_js_host())
        return DefaultHost::anim_object();
    if (auto value = maybe_call("anim", Json::make_object()))
    {
        if (auto object = object_from(*value))
            return object;
    }
    return DefaultHost::anim_object();
}

auto WasmHost::create_object(std::string_view class_name) -> ObjectPtr
{
    if (has_js_host())
    {
        auto payload = Json::make_object();
        payload.set("className", Json{ std::string{ class_name } });
        if (auto value = maybe_call("createObject", std::move(payload)))
        {
            if (auto object = object_from(*value))
                return object;
        }
    }
    return DefaultHost::create_object(class_name);
}

} // namespace gsc::wasm
