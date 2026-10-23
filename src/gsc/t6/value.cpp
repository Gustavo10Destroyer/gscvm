// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/value.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_set>

namespace gsc::t6
{

namespace
{

auto fail(std::string_view expected, std::string_view actual) -> void
{
    throw Error(std::format("value type mismatch: expected {}, got {}", expected, actual));
}

auto float_string(float value) -> std::string
{
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0.0f ? "-inf" : "inf";
    if (value == static_cast<float>(static_cast<std::int32_t>(value)) && std::abs(value) < 1.0e9f)
        return std::format("{}", static_cast<std::int32_t>(value));
    return std::format("{:.7g}", value);
}

constexpr std::size_t debug_max_depth = 8;
constexpr std::size_t debug_max_items = 32;

auto indent(std::size_t depth) -> std::string
{
    return std::string(depth * 2, ' ');
}

auto quoted(std::string_view value) -> std::string
{
    auto out = std::string{ "\"" };

    for (auto const ch : value)
    {
        switch (ch)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                    out += std::format("\\x{:02x}", static_cast<unsigned char>(ch));
                else
                    out.push_back(ch);
                break;
        }
    }

    out.push_back('"');
    return out;
}

auto format_debug_value(Value const& value, std::size_t depth, std::unordered_set<void const*>& visiting) -> std::string;

auto format_key(Value const& key) -> std::string
{
    if (key.is_string() || key.is_istring() || key.is_hash())
        return quoted(key.as_string());
    return key.to_string();
}

auto format_object_key(Value const& key) -> std::string
{
    if (key.is_string() || key.is_istring() || key.is_hash())
        return key.as_string();
    return key.to_string();
}

auto format_object_body(Object const& object, std::size_t depth, std::unordered_set<void const*>& visiting) -> std::string
{
    auto const keys = object.keys();
    if (keys.empty())
        return "{}";

    if (depth >= debug_max_depth)
        return "{...}";

    if (!visiting.insert(&object).second)
        return "{...}";

    auto out = std::string{ "{\n" };

    for (std::size_t i = 0; i < keys.size(); i++)
    {
        if (i >= debug_max_items)
        {
            out += std::format("{}... ({} more)\n", indent(depth + 1), keys.size() - i);
            break;
        }

        auto const value = object.get_index(keys[i]);
        out += std::format("{}{}: {}\n", indent(depth + 1), format_object_key(keys[i]), format_debug_value(value, depth + 1, visiting));
    }

    visiting.erase(&object);
    out += indent(depth);
    out.push_back('}');
    return out;
}

auto format_array_body(Array const& array, std::size_t depth, std::unordered_set<void const*>& visiting) -> std::string
{
    auto const keys = array.keys();
    if (keys.empty())
        return "[]";

    if (depth >= debug_max_depth)
        return "[...]";

    if (!visiting.insert(&array).second)
        return "[...]";

    auto out = std::string{ "[\n" };

    for (std::size_t i = 0; i < keys.size(); i++)
    {
        if (i >= debug_max_items)
        {
            out += std::format("{}... ({} more)\n", indent(depth + 1), keys.size() - i);
            break;
        }

        auto const value = array.get(keys[i]);
        out += std::format("{}{}: {}\n", indent(depth + 1), format_key(keys[i]), format_debug_value(value, depth + 1, visiting));
    }

    visiting.erase(&array);
    out += indent(depth);
    out.push_back(']');
    return out;
}

auto format_debug_value(Value const& value, std::size_t depth, std::unordered_set<void const*>& visiting) -> std::string
{
    switch (value.type())
    {
        case Value::Type::undefined:
            return "undefined";
        case Value::Type::string:
        case Value::Type::istring:
        case Value::Type::hash:
            return std::format("{} = {}", value.type_name(), quoted(value.as_string()));
        case Value::Type::array:
        {
            auto const& array = value.as_array();
            return std::format("array(size={}) {}", array.size(), format_array_body(array, depth, visiting));
        }
        case Value::Type::object:
        {
            auto const& object = value.as_object();
            return std::format("{} {}", object.class_name(), format_object_body(object, depth, visiting));
        }
        default:
            return std::format("{} = {}", value.type_name(), value.to_string());
    }
}

} // namespace

Value::Value() = default;
Value::~Value() = default;
Value::Value(Value const&) = default;
Value::Value(Value&&) noexcept = default;
auto Value::operator=(Value const&) -> Value& = default;
auto Value::operator=(Value&&) noexcept -> Value& = default;

auto Value::undefined() -> Value { return Value{}; }

auto Value::boolean(bool value) -> Value
{
    Value out;
    out.value_ = value;
    return out;
}

auto Value::integer(std::int32_t value) -> Value
{
    Value out;
    out.value_ = value;
    return out;
}

auto Value::floating(float value) -> Value
{
    Value out;
    out.value_ = value;
    return out;
}

auto Value::string(std::string value) -> Value
{
    Value out;
    out.value_ = StringData{ std::move(value), Type::string };
    return out;
}

auto Value::istring(std::string value) -> Value
{
    Value out;
    out.value_ = StringData{ std::move(value), Type::istring };
    return out;
}

auto Value::hash(std::string value) -> Value
{
    Value out;
    out.value_ = StringData{ std::move(value), Type::hash };
    return out;
}

auto Value::vector(Vector3 value) -> Value
{
    Value out;
    out.value_ = value;
    return out;
}

auto Value::vector(float x, float y, float z) -> Value
{
    return vector(Vector3{ x, y, z });
}

auto Value::array() -> Value { return array(std::make_shared<Array>()); }

auto Value::array(ArrayPtr value) -> Value
{
    Value out;
    out.value_ = value ? std::move(value) : std::make_shared<Array>();
    return out;
}

auto Value::object(ObjectPtr value) -> Value
{
    Value out;
    out.value_ = std::move(value);
    return out;
}

auto Value::function(FunctionRef value) -> Value
{
    Value out;
    out.value_ = std::move(value);
    return out;
}

auto Value::thread(ThreadRef value) -> Value
{
    Value out;
    out.value_ = value;
    return out;
}

auto Value::animtree(std::string name) -> Value
{
    Value out;
    out.value_ = AnimTreeRef{ std::move(name) };
    return out;
}

auto Value::animation(std::string tree, std::string name) -> Value
{
    Value out;
    out.value_ = AnimationRef{ std::move(tree), std::move(name) };
    return out;
}

auto Value::type() const -> Type
{
    switch (value_.index())
    {
        case 0: return Type::undefined;
        case 1: return Type::boolean;
        case 2: return Type::integer;
        case 3: return Type::floating;
        case 4: return std::get<StringData>(value_).type;
        case 5: return Type::vector;
        case 6: return Type::array;
        case 7: return Type::object;
        case 8: return Type::function;
        case 9: return Type::thread;
        case 10: return Type::animtree;
        case 11: return Type::animation;
        default: return Type::undefined;
    }
}

auto Value::type_name() const -> std::string_view
{
    switch (type())
    {
        case Type::undefined: return "undefined";
        case Type::boolean: return "bool";
        case Type::integer: return "int";
        case Type::floating: return "float";
        case Type::string: return "string";
        case Type::istring: return "istring";
        case Type::hash: return "hash";
        case Type::vector: return "vector";
        case Type::array: return "array";
        case Type::object: return "object";
        case Type::function: return "function";
        case Type::thread: return "thread";
        case Type::animtree: return "animtree";
        case Type::animation: return "animation";
    }

    return "undefined";
}

auto Value::is_undefined() const -> bool { return type() == Type::undefined; }
auto Value::is_boolean() const -> bool { return type() == Type::boolean; }
auto Value::is_integer() const -> bool { return type() == Type::integer; }
auto Value::is_floating() const -> bool { return type() == Type::floating; }
auto Value::is_number() const -> bool { return is_integer() || is_floating(); }
auto Value::is_string() const -> bool { return type() == Type::string; }
auto Value::is_istring() const -> bool { return type() == Type::istring; }
auto Value::is_hash() const -> bool { return type() == Type::hash; }
auto Value::is_vector() const -> bool { return type() == Type::vector; }
auto Value::is_array() const -> bool { return type() == Type::array && std::get<ArrayPtr>(value_) != nullptr; }
auto Value::is_object() const -> bool { return type() == Type::object && std::get<ObjectPtr>(value_) != nullptr; }
auto Value::is_function() const -> bool { return type() == Type::function; }
auto Value::is_thread() const -> bool { return type() == Type::thread; }
auto Value::is_animtree() const -> bool { return type() == Type::animtree; }
auto Value::is_animation() const -> bool { return type() == Type::animation; }

auto Value::as_boolean() const -> bool
{
    if (!is_boolean()) fail("bool", type_name());
    return std::get<bool>(value_);
}

auto Value::as_integer() const -> std::int32_t
{
    if (!is_integer()) fail("int", type_name());
    return std::get<std::int32_t>(value_);
}

auto Value::as_floating() const -> float
{
    if (!is_floating()) fail("float", type_name());
    return std::get<float>(value_);
}

auto Value::as_string() const -> std::string const&
{
    if (!is_string() && !is_istring() && !is_hash()) fail("string", type_name());
    return std::get<StringData>(value_).value;
}

auto Value::as_vector() const -> Vector3 const&
{
    if (!is_vector()) fail("vector", type_name());
    return std::get<Vector3>(value_);
}

auto Value::as_array() const -> Array&
{
    if (!is_array()) fail("array", type_name());
    return *std::get<ArrayPtr>(value_);
}

auto Value::as_array_ptr() const -> ArrayPtr const&
{
    if (!is_array()) fail("array", type_name());
    return std::get<ArrayPtr>(value_);
}

auto Value::as_object() const -> Object&
{
    if (!is_object()) fail("object", type_name());
    return *std::get<ObjectPtr>(value_);
}

auto Value::as_object_ptr() const -> ObjectPtr const&
{
    if (!is_object()) fail("object", type_name());
    return std::get<ObjectPtr>(value_);
}

auto Value::as_function() const -> FunctionRef const&
{
    if (!is_function()) fail("function", type_name());
    return std::get<FunctionRef>(value_);
}

auto Value::as_thread() const -> ThreadRef const&
{
    if (!is_thread()) fail("thread", type_name());
    return std::get<ThreadRef>(value_);
}

auto Value::as_animtree() const -> AnimTreeRef const&
{
    if (!is_animtree()) fail("animtree", type_name());
    return std::get<AnimTreeRef>(value_);
}

auto Value::as_animation() const -> AnimationRef const&
{
    if (!is_animation()) fail("animation", type_name());
    return std::get<AnimationRef>(value_);
}

auto Value::to_integer() const -> std::int32_t
{
    switch (type())
    {
        case Type::undefined: return 0;
        case Type::boolean: return std::get<bool>(value_) ? 1 : 0;
        case Type::integer: return std::get<std::int32_t>(value_);
        case Type::floating:
        {
            auto const value = std::get<float>(value_);
            if (!std::isfinite(value)) return 0;
            if (value >= static_cast<float>(std::numeric_limits<std::int32_t>::max())) return std::numeric_limits<std::int32_t>::max();
            if (value <= static_cast<float>(std::numeric_limits<std::int32_t>::min())) return std::numeric_limits<std::int32_t>::min();
            return static_cast<std::int32_t>(value);
        }
        case Type::string:
        case Type::istring:
        case Type::hash:
        {
            try
            {
                return static_cast<std::int32_t>(std::stoll(std::get<StringData>(value_).value, nullptr, 0));
            }
            catch (...)
            {
                return 0;
            }
        }
        default:
            return 1;
    }
}

auto Value::to_floating() const -> float
{
    switch (type())
    {
        case Type::undefined: return 0.0f;
        case Type::boolean: return std::get<bool>(value_) ? 1.0f : 0.0f;
        case Type::integer: return static_cast<float>(std::get<std::int32_t>(value_));
        case Type::floating: return std::get<float>(value_);
        case Type::string:
        case Type::istring:
        case Type::hash:
        {
            try
            {
                return std::stof(std::get<StringData>(value_).value);
            }
            catch (...)
            {
                return 0.0f;
            }
        }
        default:
            return 1.0f;
    }
}

auto Value::to_boolean() const -> bool
{
    switch (type())
    {
        case Type::undefined: return false;
        case Type::boolean: return std::get<bool>(value_);
        case Type::integer: return std::get<std::int32_t>(value_) != 0;
        case Type::floating: return std::get<float>(value_) != 0.0f;
        case Type::string:
        case Type::istring:
        case Type::hash: return !std::get<StringData>(value_).value.empty();
        case Type::thread: return std::get<ThreadRef>(value_).id != 0;
        default: return true;
    }
}

auto Value::to_string() const -> std::string
{
    switch (type())
    {
        case Type::undefined: return "undefined";
        case Type::boolean: return std::get<bool>(value_) ? "true" : "false";
        case Type::integer: return std::format("{}", std::get<std::int32_t>(value_));
        case Type::floating: return float_string(std::get<float>(value_));
        case Type::string:
        case Type::istring:
        case Type::hash: return std::get<StringData>(value_).value;
        case Type::vector:
        {
            auto const& v = std::get<Vector3>(value_);
            return std::format("({}, {}, {})", float_string(v.x), float_string(v.y), float_string(v.z));
        }
        case Type::array:
            return std::format("array#{}", static_cast<void const*>(std::get<ArrayPtr>(value_).get()));
        case Type::object:
        {
            auto const& object = std::get<ObjectPtr>(value_);
            return object ? object->to_string() : std::string{ "object" };
        }
        case Type::function:
        {
            auto const& fn = std::get<FunctionRef>(value_);
            return fn.space.empty() ? std::format("function {}", fn.name) : std::format("function {}::{}", fn.space, fn.name);
        }
        case Type::thread: return std::format("thread#{}", std::get<ThreadRef>(value_).id);
        case Type::animtree: return std::format("animtree {}", std::get<AnimTreeRef>(value_).name);
        case Type::animation: return std::format("animation {}", std::get<AnimationRef>(value_).name);
    }

    return {};
}

auto Value::to_display_string() const -> std::string
{
    return to_string();
}

auto Value::equals(Value const& other, bool strict) const -> bool
{
    auto const lhs_type = type();
    auto const rhs_type = other.type();

    if (strict && lhs_type != rhs_type)
        return false;

    if (is_number() && other.is_number())
    {
        if (is_integer() && other.is_integer())
            return as_integer() == other.as_integer();
        return to_floating() == other.to_floating();
    }

    if (lhs_type != rhs_type)
    {
        if ((is_string() || is_istring() || is_hash()) && (other.is_string() || other.is_istring() || other.is_hash()))
            return as_string() == other.as_string();
        return false;
    }

    switch (lhs_type)
    {
        case Type::undefined: return true;
        case Type::boolean: return as_boolean() == other.as_boolean();
        case Type::integer: return as_integer() == other.as_integer();
        case Type::floating: return as_floating() == other.as_floating();
        case Type::string:
        case Type::istring:
        case Type::hash: return as_string() == other.as_string();
        case Type::vector: return as_vector() == other.as_vector();
        case Type::array: return as_array_ptr() == other.as_array_ptr();
        case Type::object: return as_object().same_as(other.as_object());
        case Type::function: return as_function() == other.as_function();
        case Type::thread: return as_thread() == other.as_thread();
        case Type::animtree: return as_animtree() == other.as_animtree();
        case Type::animation: return as_animation() == other.as_animation();
    }

    return false;
}

// ---------------------------------------------------------------------------
// Array
// ---------------------------------------------------------------------------

auto Array::make_key(Value const& value) -> std::optional<Key>
{
    switch (value.type())
    {
        case Value::Type::integer:
            return Key{ value.as_integer() };
        case Value::Type::floating:
            return Key{ static_cast<std::int32_t>(value.as_floating()) };
        case Value::Type::boolean:
            return Key{ value.as_boolean() ? 1 : 0 };
        case Value::Type::string:
        case Value::Type::istring:
        case Value::Type::hash:
            return Key{ value.as_string() };
        default:
            return std::nullopt;
    }
}

auto Array::key_value(Key const& key) -> Value
{
    if (std::holds_alternative<std::int32_t>(key))
        return Value::integer(std::get<std::int32_t>(key));
    return Value::string(std::get<std::string>(key));
}

auto Array::get(Value const& key) const -> Value
{
    if (auto const converted = make_key(key))
        return get(*converted);
    return Value::undefined();
}

auto Array::get(Key const& key) const -> Value
{
    auto const it = values_.find(key);
    return it == values_.end() ? Value::undefined() : it->second;
}

auto Array::contains(Value const& key) const -> bool
{
    if (auto const converted = make_key(key))
        return contains(*converted);
    return false;
}

auto Array::contains(Key const& key) const -> bool { return values_.contains(key); }

auto Array::set(Value const& key, Value value) -> void
{
    auto const converted = make_key(key);
    if (!converted)
        throw Error(std::format("invalid array key type '{}'", key.type_name()));
    set(*converted, std::move(value));
}

auto Array::set(Key key, Value value) -> void
{
    auto const it = values_.find(key);
    if (it == values_.end())
        order_.push_back(key);
    values_.insert_or_assign(std::move(key), std::move(value));
}

auto Array::erase(Value const& key) -> bool
{
    if (auto const converted = make_key(key))
        return erase(*converted);
    return false;
}

auto Array::erase(Key const& key) -> bool
{
    if (values_.erase(key) == 0)
        return false;
    order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
    return true;
}

auto Array::clear() -> void
{
    values_.clear();
    order_.clear();
}

auto Array::size() const -> std::size_t { return values_.size(); }
auto Array::empty() const -> bool { return values_.empty(); }

auto Array::first_key() const -> Value
{
    if (order_.empty())
        return Value::undefined();
    return key_value(order_.front());
}

auto Array::next_key(Value const& key) const -> Value
{
    auto const converted = make_key(key);
    if (!converted)
        return Value::undefined();

    auto const it = std::find(order_.begin(), order_.end(), *converted);
    if (it == order_.end() || std::next(it) == order_.end())
        return Value::undefined();
    return key_value(*std::next(it));
}

auto Array::keys() const -> std::vector<Value>
{
    auto out = std::vector<Value>{};
    out.reserve(order_.size());
    for (auto const& key : order_)
        out.push_back(key_value(key));
    return out;
}

// ---------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------

auto Object::debug_name() const -> std::string
{
    return std::string(class_name());
}

auto Object::to_string() const -> std::string
{
    return debug_name();
}

auto Object::get_field(std::string_view) const -> Value { return Value::undefined(); }
auto Object::set_field(std::string_view, Value) -> void {}
auto Object::clear_field(std::string_view) -> bool { return false; }
auto Object::has_field(std::string_view) const -> bool { return false; }

auto Object::get_index(Value const& key) const -> Value
{
    if (key.is_string() || key.is_istring() || key.is_hash())
        return get_field(key.as_string());
    return Value::undefined();
}

auto Object::set_index(Value const& key, Value value) -> void
{
    if (key.is_string() || key.is_istring() || key.is_hash())
        set_field(key.as_string(), std::move(value));
    else
        throw Error(std::format("object field names must be strings, got '{}'", key.type_name()));
}

auto Object::clear_index(Value const& key) -> bool
{
    if (key.is_string() || key.is_istring() || key.is_hash())
        return clear_field(key.as_string());
    return false;
}

auto Object::size() const -> std::size_t { return 0; }
auto Object::first_key() const -> Value { return Value::undefined(); }
auto Object::next_key(Value const&) const -> Value { return Value::undefined(); }
auto Object::keys() const -> std::vector<Value> { return {}; }
auto Object::same_as(Object const& other) const -> bool { return this == &other; }

// ---------------------------------------------------------------------------
// ScriptObject
// ---------------------------------------------------------------------------

ScriptObject::ScriptObject(std::string class_name) : class_name_(std::move(class_name))
{
}

auto ScriptObject::class_name() const -> std::string_view { return class_name_; }
auto ScriptObject::debug_name() const -> std::string { return std::format("{}#{}", class_name_, static_cast<void const*>(this)); }

auto ScriptObject::to_string() const -> std::string
{
    auto visiting = std::unordered_set<void const*>{};
    return std::format("{} {}", class_name_, format_object_body(*this, 0, visiting));
}

auto ScriptObject::get_field(std::string_view name) const -> Value { return fields_.get(Value::string(std::string(name))); }
auto ScriptObject::set_field(std::string_view name, Value value) -> void { fields_.set(Value::string(std::string(name)), std::move(value)); }
auto ScriptObject::clear_field(std::string_view name) -> bool { return fields_.erase(Value::string(std::string(name))); }
auto ScriptObject::has_field(std::string_view name) const -> bool { return fields_.contains(Value::string(std::string(name))); }

auto ScriptObject::get_index(Value const& key) const -> Value { return fields_.get(key); }
auto ScriptObject::set_index(Value const& key, Value value) -> void { fields_.set(key, std::move(value)); }
auto ScriptObject::clear_index(Value const& key) -> bool { return fields_.erase(key); }

auto ScriptObject::size() const -> std::size_t { return fields_.size(); }
auto ScriptObject::first_key() const -> Value { return fields_.first_key(); }
auto ScriptObject::next_key(Value const& key) const -> Value { return fields_.next_key(key); }
auto ScriptObject::keys() const -> std::vector<Value> { return fields_.keys(); }
auto ScriptObject::same_as(Object const& other) const -> bool { return this == &other; }

} // namespace gsc::t6
