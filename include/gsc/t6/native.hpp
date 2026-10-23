// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "gsc/t6/value.hpp"

namespace gsc::t6
{

class VM;
class NativeContext;

using NativeFunction = std::function<void(NativeContext&)>;

// Per-VM native registry. Registration is intentionally separate from the VM's
// bytecode tables so that multiple VMs never share native state.
class NativeRegistry
{
public:
    auto register_function(std::string_view name, NativeFunction function) -> void;
    auto register_function(std::string_view space, std::string_view name, NativeFunction function) -> void;
    auto unregister_function(std::string_view name) -> bool;
    auto unregister_function(std::string_view space, std::string_view name) -> bool;
    auto contains(std::string_view name) const -> bool;
    auto contains(std::string_view space, std::string_view name) const -> bool;

    auto find(std::string_view space, std::string_view name) const -> NativeFunction const*;

    // Canonical "space::name" (or bare "name") entries currently registered.
    auto names() const -> std::vector<std::string>;

private:
    std::map<std::string, NativeFunction, std::less<>> functions_;
};

// Arguments are copied Values. Strings are owned copies; arrays, objects and
// threads are handles and remain valid for the duration of the call and beyond
// if the host retains them (the VM keeps them alive through shared ownership).
class NativeContext
{
public:
    ~NativeContext();
    NativeContext(NativeContext const&) = delete;
    NativeContext(NativeContext&&) noexcept;
    auto operator=(NativeContext const&) -> NativeContext& = delete;
    auto operator=(NativeContext&&) noexcept -> NativeContext&;

    auto vm() -> VM&;
    auto vm() const -> VM const&;

    auto argument_count() const -> std::size_t;
    auto argument(std::size_t index) const -> Value const&;

    template <typename T>
    auto get_argument(std::size_t index) const -> T
    {
        auto const& value = argument(index);

        if constexpr (std::is_same_v<T, Value>)
            return value;
        else if constexpr (std::is_same_v<T, std::string>)
            return std::string(value.as_string());
        else if constexpr (std::is_same_v<T, std::string_view>)
            return std::string_view(value.as_string());
        else if constexpr (std::is_same_v<T, bool>)
        {
            if (!value.is_boolean() && !value.is_integer() && !value.is_floating())
                throw Error(std::format("argument {}: expected bool, got {}", index, value.type_name()));
            return value.to_boolean();
        }
        else if constexpr (std::is_integral_v<T>)
        {
            if (!value.is_number() && !value.is_boolean())
                throw Error(std::format("argument {}: expected integer, got {}", index, value.type_name()));
            return static_cast<T>(value.to_integer());
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            if (!value.is_number() && !value.is_boolean())
                throw Error(std::format("argument {}: expected number, got {}", index, value.type_name()));
            return static_cast<T>(value.to_floating());
        }
        else if constexpr (std::is_same_v<T, Vector3>)
        {
            return value.as_vector();
        }
        else
        {
            static_assert(sizeof(T) == 0, "unsupported NativeContext::get_argument<T> type");
        }
    }

    template <typename T>
    auto try_get_argument(std::size_t index) const -> std::optional<T>
    {
        try
        {
            return get_argument<T>(index);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    auto self() const -> Value const&;
    auto return_value() const -> Value const&;
    auto set_return_value(Value value) -> void;
    auto return_undefined() -> void;

    auto function_space() const -> std::string_view;
    auto function_name() const -> std::string_view;
    auto thread_id() const -> std::uint64_t;

    [[noreturn]] auto error(std::string_view message) const -> void;

    // Internal: constructed by the VM for each native invocation. Impl is
    // intentionally opaque so embedding code cannot forge a context.
    struct Impl;
    explicit NativeContext(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace gsc::t6
