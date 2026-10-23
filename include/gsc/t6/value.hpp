// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "gsc/error.hpp"

namespace gsc::t6
{

class Array;
class Object;

using ArrayPtr = std::shared_ptr<Array>;
using ObjectPtr = std::shared_ptr<Object>;

struct Vector3
{
    float x{};
    float y{};
    float z{};

    friend auto operator==(Vector3 const&, Vector3 const&) -> bool = default;
};

// A reference to a script or native function. Script references are resolved
// lazily so that a module may be loaded after another module already holds a
// function value pointing at it.
struct FunctionRef
{
    enum class Kind : std::uint8_t
    {
        unresolved,
        script,
        native,
    };

    Kind kind{ Kind::unresolved };
    std::string module;
    std::string space;
    std::string name;
    std::uint8_t params{};
    std::uint8_t flags{};

    friend auto operator==(FunctionRef const&, FunctionRef const&) -> bool = default;
};

// A handle to a cooperative script thread. The id is stable for the lifetime of
// the VM; id 0 is never a valid thread.
struct ThreadRef
{
    std::uint64_t id{};

    explicit operator bool() const { return id != 0; }
    friend auto operator==(ThreadRef const&, ThreadRef const&) -> bool = default;
};

struct AnimTreeRef
{
    std::string name;

    friend auto operator==(AnimTreeRef const&, AnimTreeRef const&) -> bool = default;
};

struct AnimationRef
{
    std::string tree;
    std::string name;

    friend auto operator==(AnimationRef const&, AnimationRef const&) -> bool = default;
};

class Value
{
public:
    enum class Type : std::uint8_t
    {
        undefined,
        boolean,
        integer,
        floating,
        string,
        istring,
        hash,
        vector,
        array,
        object,
        function,
        thread,
        animtree,
        animation,
    };

    struct StringData
    {
        std::string value;
        Type type{ Type::string };

        friend auto operator==(StringData const&, StringData const&) -> bool = default;
    };

    Value();
    ~Value();
    Value(Value const&);
    Value(Value&&) noexcept;
    auto operator=(Value const&) -> Value&;
    auto operator=(Value&&) noexcept -> Value&;

    static auto undefined() -> Value;
    static auto boolean(bool value) -> Value;
    static auto integer(std::int32_t value) -> Value;
    static auto floating(float value) -> Value;
    static auto string(std::string value) -> Value;
    static auto istring(std::string value) -> Value;
    static auto hash(std::string value) -> Value;
    static auto vector(Vector3 value) -> Value;
    static auto vector(float x, float y, float z) -> Value;
    static auto array() -> Value;
    static auto array(ArrayPtr value) -> Value;
    static auto object(ObjectPtr value) -> Value;
    static auto function(FunctionRef value) -> Value;
    static auto thread(ThreadRef value) -> Value;
    static auto animtree(std::string name) -> Value;
    static auto animation(std::string tree, std::string name) -> Value;

    auto type() const -> Type;
    auto type_name() const -> std::string_view;

    auto is_undefined() const -> bool;
    auto is_boolean() const -> bool;
    auto is_integer() const -> bool;
    auto is_floating() const -> bool;
    auto is_number() const -> bool;
    auto is_string() const -> bool;
    auto is_istring() const -> bool;
    auto is_hash() const -> bool;
    auto is_vector() const -> bool;
    auto is_array() const -> bool;
    auto is_object() const -> bool;
    auto is_function() const -> bool;
    auto is_thread() const -> bool;
    auto is_animtree() const -> bool;
    auto is_animation() const -> bool;

    auto as_boolean() const -> bool;
    auto as_integer() const -> std::int32_t;
    auto as_floating() const -> float;
    auto as_string() const -> std::string const&;
    auto as_vector() const -> Vector3 const&;
    auto as_array() const -> Array&;
    auto as_array_ptr() const -> ArrayPtr const&;
    auto as_object() const -> Object&;
    auto as_object_ptr() const -> ObjectPtr const&;
    auto as_function() const -> FunctionRef const&;
    auto as_thread() const -> ThreadRef const&;
    auto as_animtree() const -> AnimTreeRef const&;
    auto as_animation() const -> AnimationRef const&;

    auto to_integer() const -> std::int32_t;
    auto to_floating() const -> float;
    auto to_boolean() const -> bool;

    // Serialization used for string concatenation and console formatting.
    // Objects can customize this through Object::to_string().
    auto to_string() const -> std::string;
    auto to_display_string() const -> std::string;

    auto equals(Value const& other, bool strict = false) const -> bool;

    friend auto operator==(Value const& lhs, Value const& rhs) -> bool
    {
        return lhs.equals(rhs);
    }

private:
    using Storage = std::variant<
        std::monostate,
        bool,
        std::int32_t,
        float,
        StringData,
        Vector3,
        ArrayPtr,
        ObjectPtr,
        FunctionRef,
        ThreadRef,
        AnimTreeRef,
        AnimationRef>;

    Storage value_;
};

// An associative GSC array. Values are handles: copying a Value copies the
// handle, not the contents, which matches GSC's reference-like array semantics.
class Array
{
public:
    using Key = std::variant<std::int32_t, std::string>;

    Array() = default;

    auto get(Value const& key) const -> Value;
    auto get(Key const& key) const -> Value;
    auto contains(Value const& key) const -> bool;
    auto contains(Key const& key) const -> bool;
    auto set(Value const& key, Value value) -> void;
    auto set(Key key, Value value) -> void;
    auto erase(Value const& key) -> bool;
    auto erase(Key const& key) -> bool;
    auto clear() -> void;
    auto size() const -> std::size_t;
    auto empty() const -> bool;

    auto first_key() const -> Value;
    auto next_key(Value const& key) const -> Value;
    auto keys() const -> std::vector<Value>;

    static auto make_key(Value const& value) -> std::optional<Key>;
    static auto key_value(Key const& key) -> Value;

private:
    std::map<Key, Value> values_;
    std::vector<Key> order_;
};

// Host-visible script object. The default implementation is a field container;
// a host may subclass it to expose entities or engine objects.
class Object
{
public:
    virtual ~Object() = default;

    virtual auto class_name() const -> std::string_view { return "object"; }
    virtual auto debug_name() const -> std::string;

    // Custom serialization hook for native objects and C++ bindings.
    virtual auto to_string() const -> std::string;

    virtual auto get_field(std::string_view name) const -> Value;
    virtual auto set_field(std::string_view name, Value value) -> void;
    virtual auto clear_field(std::string_view name) -> bool;
    virtual auto has_field(std::string_view name) const -> bool;

    virtual auto get_index(Value const& key) const -> Value;
    virtual auto set_index(Value const& key, Value value) -> void;
    virtual auto clear_index(Value const& key) -> bool;

    virtual auto size() const -> std::size_t;
    virtual auto first_key() const -> Value;
    virtual auto next_key(Value const& key) const -> Value;
    virtual auto keys() const -> std::vector<Value>;

    virtual auto same_as(Object const& other) const -> bool;
};

// Simple field container used by the standalone host for level/game/self.
class ScriptObject final : public Object
{
public:
    explicit ScriptObject(std::string class_name = "object");

    auto class_name() const -> std::string_view override;
    auto debug_name() const -> std::string override;
    auto to_string() const -> std::string override;

    auto get_field(std::string_view name) const -> Value override;
    auto set_field(std::string_view name, Value value) -> void override;
    auto clear_field(std::string_view name) -> bool override;
    auto has_field(std::string_view name) const -> bool override;

    auto get_index(Value const& key) const -> Value override;
    auto set_index(Value const& key, Value value) -> void override;
    auto clear_index(Value const& key) -> bool override;

    auto size() const -> std::size_t override;
    auto first_key() const -> Value override;
    auto next_key(Value const& key) const -> Value override;
    auto keys() const -> std::vector<Value> override;

    auto same_as(Object const& other) const -> bool override;

private:
    std::string class_name_;
    Array fields_;
};

} // namespace gsc::t6
