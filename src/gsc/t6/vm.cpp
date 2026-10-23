// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/vm.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include "gsc/t6/compiler.hpp"
#include "gsc/t6/module.hpp"

#include "xsk/arc/common/assembly.hpp"

namespace gsc::t6
{

namespace
{

using detail::Function;
using detail::Instruction;
using detail::Module;
using detail::SwitchEntry;

constexpr std::uint8_t import_developer = 0x10;
[[maybe_unused]] constexpr std::uint8_t import_unscoped = 0x20;
[[maybe_unused]] constexpr std::uint8_t function_varargs = 0x20;

// GSC stores fractional literals as 32-bit floats. A small absolute tolerance
// keeps exact host ticks such as 0.1 from waking a 0.1-second wait late.
constexpr double time_epsilon = 1e-6;

auto lower(std::string_view value) -> std::string
{
    auto out = std::string{ value };
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

auto normalize_path(std::string_view name) -> std::string
{
    auto out = lower(name);
    std::replace(out.begin(), out.end(), '\\', '/');

    while (out.starts_with("./"))
        out.erase(0, 2);

    for (auto const ext : { ".gsc", ".gscc", ".csc", ".gscbin", ".bin" })
    {
        if (out.ends_with(ext))
        {
            out.resize(out.size() - std::string_view{ ext }.size());
            break;
        }
    }

    return out;
}

auto basename(std::string_view path) -> std::string
{
    auto const pos = path.find_last_of('/');
    if (pos == std::string_view::npos)
        return std::string{ path };
    return std::string{ path.substr(pos + 1) };
}

auto parent_path(std::string_view path) -> std::string
{
    auto const pos = path.find_last_of('/');
    if (pos == std::string_view::npos)
        return {};
    return std::string{ path.substr(0, pos) };
}

// Unlike normalize_path this keeps extensions because virtual files are also
// used for #include/#using headers (.gsh).
auto normalize_virtual_path(std::string_view path) -> std::string
{
    auto out = std::string{ path };
    std::replace(out.begin(), out.end(), '\\', '/');

    while (out.starts_with("./"))
        out.erase(0, 2);
    while (out.starts_with("/"))
        out.erase(0, 1);
    while (!out.empty() && out.back() == '/')
        out.pop_back();

    return out;
}

auto path_join(std::string_view left, std::string_view right) -> std::string
{
    if (left.empty())
        return std::string{ right };
    if (right.empty())
        return std::string{ left };
    auto out = std::string{ left };
    if (!out.ends_with('/'))
        out.push_back('/');
    out.append(right);
    return out;
}

auto parse_int(std::string_view value) -> std::int32_t
{
    auto result = std::int32_t{};
    auto const parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{})
    {
        try
        {
            return static_cast<std::int32_t>(std::stoll(std::string{ value }, nullptr, 0));
        }
        catch (...)
        {
            throw Error(std::format("invalid integer operand '{}'", value));
        }
    }
    return result;
}

auto parse_size(std::string_view value) -> std::size_t
{
    auto result = std::size_t{};
    auto const parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{})
        throw Error(std::format("invalid size operand '{}'", value));
    return result;
}

auto parse_float(std::string_view value) -> float
{
    try
    {
        return std::stof(std::string{ value });
    }
    catch (...)
    {
        return static_cast<float>(parse_int(value));
    }
}

struct Import
{
    std::string space;
    std::string name;
    std::uint8_t params{};
    std::uint8_t flags{};

    auto is_method() const -> bool { return (flags & 0x0F) == 4 || (flags & 0x0F) == 5; }
    auto is_thread() const -> bool { return (flags & 0x0F) == 3 || (flags & 0x0F) == 5; }
    auto is_reference() const -> bool { return (flags & 0x0F) == 1; }
    auto is_developer() const -> bool { return (flags & import_developer) != 0; }
};

auto parse_import(Instruction const& instruction, bool pointer = false) -> Import
{
    if (pointer)
    {
        if (instruction.data.size() < 1)
            throw Error(std::format("{} at 0x{:X} has no argument count", instruction.name, instruction.offset));

        auto import = Import{};
        import.params = static_cast<std::uint8_t>(parse_int(instruction.data[0]));
        return import;
    }

    if (instruction.data.size() < 4)
        throw Error(std::format("{} at 0x{:X} has a truncated import", instruction.name, instruction.offset));

    return Import{
        .space = instruction.data[0],
        .name = instruction.data[1],
        .params = static_cast<std::uint8_t>(parse_int(instruction.data[2])),
        .flags = static_cast<std::uint8_t>(parse_int(instruction.data[3])),
    };
}

auto is_string_like(Value const& value) -> bool
{
    return value.is_string() || value.is_istring() || value.is_hash();
}

auto is_numeric(Value const& value) -> bool
{
    return value.is_number() || value.is_boolean();
}

auto as_double(Value const& value) -> double
{
    if (value.is_integer())
        return static_cast<double>(value.as_integer());
    if (value.is_floating())
        return static_cast<double>(value.as_floating());
    if (value.is_boolean())
        return value.as_boolean() ? 1.0 : 0.0;
    throw Error(std::format("expected a number, got {}", value.type_name()));
}

auto vector_of(Value const& value) -> Vector3
{
    if (value.is_vector())
        return value.as_vector();
    if (is_numeric(value))
    {
        auto const n = static_cast<float>(as_double(value));
        return Vector3{ n, n, n };
    }
    throw Error(std::format("expected a vector, got {}", value.type_name()));
}

auto numeric_result(double value, bool force_float) -> Value
{
    if (!force_float && std::fabs(value) <= static_cast<double>(std::numeric_limits<std::int32_t>::max()) && std::floor(value) == value)
        return Value::integer(static_cast<std::int32_t>(value));
    return Value::floating(static_cast<float>(value));
}

auto add_values(Value const& lhs, Value const& rhs) -> Value
{
    if (is_string_like(lhs) || is_string_like(rhs))
        return Value::string(lhs.to_string() + rhs.to_string());

    if (lhs.is_vector() || rhs.is_vector())
    {
        auto const a = vector_of(lhs);
        auto const b = vector_of(rhs);
        return Value::vector(a.x + b.x, a.y + b.y, a.z + b.z);
    }

    if (lhs.is_integer() && rhs.is_integer())
        return numeric_result(static_cast<double>(lhs.as_integer()) + rhs.as_integer(), false);

    return numeric_result(as_double(lhs) + as_double(rhs), lhs.is_floating() || rhs.is_floating());
}

auto subtract_values(Value const& lhs, Value const& rhs) -> Value
{
    if (lhs.is_vector() || rhs.is_vector())
    {
        auto const a = vector_of(lhs);
        auto const b = vector_of(rhs);
        return Value::vector(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    if (lhs.is_integer() && rhs.is_integer())
        return numeric_result(static_cast<double>(lhs.as_integer()) - rhs.as_integer(), false);

    return numeric_result(as_double(lhs) - as_double(rhs), lhs.is_floating() || rhs.is_floating());
}

auto multiply_values(Value const& lhs, Value const& rhs) -> Value
{
    if (lhs.is_vector() || rhs.is_vector())
    {
        auto const a = vector_of(lhs);
        auto const b = vector_of(rhs);
        return Value::vector(a.x * b.x, a.y * b.y, a.z * b.z);
    }

    if (lhs.is_integer() && rhs.is_integer())
        return numeric_result(static_cast<double>(lhs.as_integer()) * rhs.as_integer(), false);

    return numeric_result(as_double(lhs) * as_double(rhs), lhs.is_floating() || rhs.is_floating());
}

auto divide_values(Value const& lhs, Value const& rhs) -> Value
{
    if (lhs.is_vector() || rhs.is_vector())
    {
        auto const a = vector_of(lhs);
        auto const b = vector_of(rhs);
        if (b.x == 0.0f || b.y == 0.0f || b.z == 0.0f)
            throw Error("division by zero");
        return Value::vector(a.x / b.x, a.y / b.y, a.z / b.z);
    }

    auto const divisor = as_double(rhs);
    if (divisor == 0.0)
        throw Error("division by zero");

    // GSC's '/' always produces a float.
    return Value::floating(static_cast<float>(as_double(lhs) / divisor));
}

auto modulus_values(Value const& lhs, Value const& rhs) -> Value
{
    auto const divisor = rhs.to_integer();
    if (divisor == 0)
        throw Error("modulus by zero");
    return Value::integer(lhs.to_integer() % divisor);
}

auto compare_values(Value const& lhs, Value const& rhs) -> int
{
    if (is_string_like(lhs) && is_string_like(rhs))
    {
        auto const a = lhs.as_string();
        auto const b = rhs.as_string();
        return a < b ? -1 : (a > b ? 1 : 0);
    }

    if (lhs.is_vector() || rhs.is_vector())
        throw Error("cannot order vector values");

    auto const a = as_double(lhs);
    auto const b = as_double(rhs);
    return a < b ? -1 : (a > b ? 1 : 0);
}

auto apply_binary(xsk::arc::opcode op, Value const& lhs, Value const& rhs) -> Value
{
    using xsk::arc::opcode;

    switch (op)
    {
        case opcode::OP_Plus:
            return add_values(lhs, rhs);
        case opcode::OP_Minus:
            return subtract_values(lhs, rhs);
        case opcode::OP_Multiply:
            return multiply_values(lhs, rhs);
        case opcode::OP_Divide:
            return divide_values(lhs, rhs);
        case opcode::OP_Modulus:
            return modulus_values(lhs, rhs);
        case opcode::OP_Bit_Or:
            return Value::integer(lhs.to_integer() | rhs.to_integer());
        case opcode::OP_Bit_Xor:
            return Value::integer(lhs.to_integer() ^ rhs.to_integer());
        case opcode::OP_Bit_And:
            return Value::integer(lhs.to_integer() & rhs.to_integer());
        case opcode::OP_ShiftLeft:
            return Value::integer(lhs.to_integer() << (rhs.to_integer() & 31));
        case opcode::OP_ShiftRight:
            return Value::integer(lhs.to_integer() >> (rhs.to_integer() & 31));
        case opcode::OP_Equal:
            return Value::integer(lhs.equals(rhs) ? 1 : 0);
        case opcode::OP_NotEqual:
            return Value::integer(lhs.equals(rhs) ? 0 : 1);
        case opcode::OP_SuperEqual:
            return Value::integer(lhs.equals(rhs, true) ? 1 : 0);
        case opcode::OP_SuperNotEqual:
            return Value::integer(lhs.equals(rhs, true) ? 0 : 1);
        case opcode::OP_LessThan:
            return Value::integer(compare_values(lhs, rhs) < 0 ? 1 : 0);
        case opcode::OP_GreaterThan:
            return Value::integer(compare_values(lhs, rhs) > 0 ? 1 : 0);
        case opcode::OP_LessThanOrEqualTo:
            return Value::integer(compare_values(lhs, rhs) <= 0 ? 1 : 0);
        case opcode::OP_GreaterThanOrEqualTo:
            return Value::integer(compare_values(lhs, rhs) >= 0 ? 1 : 0);
        default:
            throw Error(std::format("opcode 0x{:02X} is not a binary operator", static_cast<unsigned>(op)));
    }
}

auto angles_to_forward(Vector3 const& angles) -> Vector3
{
    auto const pitch = static_cast<double>(angles.x) * std::numbers::pi / 180.0;
    auto const yaw = static_cast<double>(angles.y) * std::numbers::pi / 180.0;

    return Vector3{
        static_cast<float>(std::cos(pitch) * std::cos(yaw)),
        static_cast<float>(std::cos(pitch) * std::sin(yaw)),
        static_cast<float>(-std::sin(pitch)),
    };
}

auto angles_to_right(Vector3 const& angles) -> Vector3
{
    auto const yaw = static_cast<double>(angles.y) * std::numbers::pi / 180.0;
    return Vector3{ static_cast<float>(std::sin(yaw)), static_cast<float>(-std::cos(yaw)), 0.0f };
}

auto angles_to_up(Vector3 const& angles) -> Vector3
{
    auto const forward = angles_to_forward(angles);
    auto const right = angles_to_right(angles);

    return Vector3{
        right.y * forward.z - right.z * forward.y,
        right.z * forward.x - right.x * forward.z,
        right.x * forward.y - right.y * forward.x,
    };
}

auto vector_to_angles(Vector3 const& vector) -> Vector3
{
    auto const length = std::sqrt(static_cast<double>(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z));
    if (length == 0.0)
        return Vector3{};

    auto const pitch = -std::asin(static_cast<double>(vector.z) / length) * 180.0 / std::numbers::pi;
    auto const yaw = std::atan2(static_cast<double>(vector.y), static_cast<double>(vector.x)) * 180.0 / std::numbers::pi;
    return Vector3{ static_cast<float>(pitch), static_cast<float>(yaw), 0.0f };
}

auto clamp_angle_180(float value) -> float
{
    auto angle = std::fmod(value + 180.0f, 360.0f);
    if (angle < 0.0f)
        angle += 360.0f;
    return angle - 180.0f;
}

auto wildcard_match(std::string_view pattern, std::string_view value) -> bool
{
    // waittillmatch event patterns are case-insensitive and commonly use '*' as
    // a suffix wildcard. Supporting a prefix/suffix pair covers the practical
    // cases without pretending to implement the engine's full matcher.
    auto const p = lower(pattern);
    auto const v = lower(value);
    auto const star = p.find('*');

    if (star == std::string::npos)
        return p == v;

    auto const prefix = std::string_view{ p }.substr(0, star);
    auto const suffix = std::string_view{ p }.substr(star + 1);
    return v.size() >= prefix.size() + suffix.size() && v.starts_with(prefix) && v.ends_with(suffix);
}

struct LValue;
struct Thread;

struct StackItem
{
    enum class Kind : std::uint8_t
    {
        value,
        marker,
        reference,
    };

    Kind kind{ Kind::value };
    Value value;
    std::shared_ptr<LValue> reference;

    static auto make(Value v) -> StackItem
    {
        auto item = StackItem{};
        item.value = std::move(v);
        return item;
    }

    static auto make_marker() -> StackItem
    {
        auto item = StackItem{};
        item.kind = Kind::marker;
        return item;
    }

    static auto make_reference(std::shared_ptr<LValue> ref) -> StackItem
    {
        auto item = StackItem{};
        item.kind = Kind::reference;
        item.reference = std::move(ref);
        return item;
    }
};

struct Frame
{
    std::shared_ptr<Module> module;
    Function const* function{};
    std::size_t pc{};
    std::vector<StackItem> stack;
    std::vector<Value> locals;
    std::vector<std::string> local_names;
    std::vector<Value> varargs;
    Value self;
    std::size_t max_stack{};
    bool locals_created{};

    auto local_slot(std::size_t index) const -> std::size_t
    {
        if (index >= locals.size())
            throw Error(std::format("local {} is out of range ({} declared)", index, locals.size()));
        return locals.size() - 1 - index;
    }

    auto local(std::size_t index) -> Value&
    {
        return locals[local_slot(index)];
    }

    auto local(std::size_t index) const -> Value const&
    {
        return locals[local_slot(index)];
    }

    auto push(StackItem item) -> void
    {
        if (max_stack != 0 && stack.size() >= max_stack)
            throw Error(std::format("stack limit of {} exceeded in '{}'", max_stack, function ? function->name : "?"));
        stack.push_back(std::move(item));
    }

    auto push(Value value) -> void
    {
        push(StackItem::make(std::move(value)));
    }

    auto pop_item() -> StackItem
    {
        if (stack.empty())
            throw Error("stack underflow");
        auto item = std::move(stack.back());
        stack.pop_back();
        return item;
    }

    auto peek_item() -> StackItem&
    {
        if (stack.empty())
            throw Error("stack underflow");
        return stack.back();
    }

    auto pop_value() -> Value
    {
        auto item = pop_item();
        if (item.kind == StackItem::Kind::reference)
            throw Error("a variable reference cannot be used as an rvalue here");
        if (item.kind == StackItem::Kind::marker)
            throw Error("expected a value, found a call marker");
        return std::move(item.value);
    }

    auto pop_operand(Thread& thread) -> Value;

    auto pop_reference() -> std::shared_ptr<LValue>
    {
        auto item = pop_item();
        if (item.kind != StackItem::Kind::reference || !item.reference)
            throw Error("expected a variable reference");
        return std::move(item.reference);
    }

    auto pop_marker() -> void
    {
        auto item = pop_item();
        if (item.kind != StackItem::Kind::marker)
            throw Error("expected a call marker");
    }

    auto expect_marker() -> void
    {
        if (stack.empty() || stack.back().kind != StackItem::Kind::marker)
            throw Error("expected a call marker");
        stack.pop_back();
    }
};

struct LValue
{
    enum class Kind : std::uint8_t
    {
        local,
        array_element,
        object_field,
    };

    Kind kind{ Kind::local };
    std::size_t frame_depth{};
    std::size_t local_index{};
    ArrayPtr array;
    ObjectPtr object;
    Value key;
    std::string field;

    auto read(Thread& thread) -> Value;
    auto write(Thread& thread, Value value) -> void;
    auto clear(Thread& thread) -> void;
};

struct EndOnSubscription
{
    Value object;
    std::string event;
};

struct Thread
{
    std::uint64_t id{};
    std::vector<Frame> frames;
    ThreadStatus status{ ThreadStatus::ready };
    double wake_time{};
    std::string wait_event;
    Value wait_object;
    std::size_t wait_argc{};
    bool wait_match{};
    bool yielded{};
    std::vector<Value> delivered;
    std::vector<EndOnSubscription> endons;
    Value self;
    Value result;
    std::string error;

    auto push_frame(Frame frame) -> void { frames.push_back(std::move(frame)); }

    auto abort() -> void
    {
        status = ThreadStatus::aborted;
        wake_time = 0.0;
        wait_event.clear();
        wait_object = Value::undefined();
        wait_argc = 0;
        wait_match = false;
        delivered.clear();
        yielded = false;
    }

    auto current_frame() -> Frame&
    {
        if (frames.empty())
            throw Error("thread has no active frame");
        return frames.back();
    }
};

auto Frame::pop_operand(Thread& thread) -> Value
{
    auto item = pop_item();
    if (item.kind == StackItem::Kind::reference)
        return item.reference->read(thread);
    if (item.kind == StackItem::Kind::marker)
        throw Error("expected a value, found a call marker");
    return std::move(item.value);
}

auto LValue::read(Thread& thread) -> Value
{
    switch (kind)
    {
        case Kind::local:
        {
            if (frame_depth >= thread.frames.size())
                throw Error("stale local reference");
            return thread.frames[frame_depth].local(local_index);
        }
        case Kind::array_element:
            if (array)
                return array->get(key);
            if (object)
                return object->get_index(key);
            throw Error("stale array reference");
        case Kind::object_field:
            if (!object)
                throw Error("stale field reference");
            return object->get_field(field);
    }

    throw Error("invalid reference");
}

auto LValue::write(Thread& thread, Value value) -> void
{
    switch (kind)
    {
        case Kind::local:
        {
            if (frame_depth >= thread.frames.size())
                throw Error("stale local reference");
            thread.frames[frame_depth].local(local_index) = std::move(value);
            return;
        }
        case Kind::array_element:
            if (array)
            {
                array->set(key, std::move(value));
                return;
            }
            if (object)
            {
                object->set_index(key, std::move(value));
                return;
            }
            throw Error("stale array reference");
        case Kind::object_field:
            if (!object)
                throw Error("stale field reference");
            object->set_field(field, std::move(value));
            return;
    }

    throw Error("invalid reference");
}

auto make_array_element_ref(Value const& container, Value key) -> std::shared_ptr<LValue>
{
    auto ref = std::make_shared<LValue>();
    ref->kind = LValue::Kind::array_element;
    ref->key = std::move(key);

    if (container.is_array())
        ref->array = container.as_array_ptr();
    else if (container.is_object())
        ref->object = container.as_object_ptr();
    else
        throw Error(std::format("cannot index into {}", container.type_name()));

    return ref;
}

auto make_field_ref(Value const& object, std::string field) -> std::shared_ptr<LValue>
{
    if (!object.is_object())
        throw Error(std::format("cannot access field '{}' on {}", field, object.type_name()));

    auto ref = std::make_shared<LValue>();
    ref->kind = LValue::Kind::object_field;
    ref->object = object.as_object_ptr();
    ref->field = std::move(field);
    return ref;
}

auto make_local_ref(std::size_t frame_depth, std::size_t index) -> std::shared_ptr<LValue>
{
    auto ref = std::make_shared<LValue>();
    ref->kind = LValue::Kind::local;
    ref->frame_depth = frame_depth;
    ref->local_index = index;
    return ref;
}

} // namespace

// ---------------------------------------------------------------------------
// RuntimeError
// ---------------------------------------------------------------------------
RuntimeError::RuntimeError(std::string message, std::string module, std::string function, std::size_t offset, std::string opcode, std::uint64_t thread)
    : Error(std::format("{} [module '{}', function '{}', {} at 0x{:X}, thread {}]", message, module.empty() ? "?" : module, function.empty() ? "?" : function, opcode.empty() ? "unknown opcode" : opcode, offset, thread)),
      module_(std::move(module)),
      function_(std::move(function)),
      offset_(offset),
      opcode_(std::move(opcode)),
      thread_id_(thread)
{
}

// ---------------------------------------------------------------------------
// NativeContext
// ---------------------------------------------------------------------------
struct NativeContext::Impl
{
    VM* vm{};
    std::uint64_t thread{};
    std::string module;
    std::string function;
    std::string space;
    std::string name;
    std::string opcode;
    std::size_t offset{};
    std::vector<Value> args;
    Value self;
    Value result;
};

NativeContext::NativeContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeContext::~NativeContext() = default;
NativeContext::NativeContext(NativeContext&&) noexcept = default;
auto NativeContext::operator=(NativeContext&&) noexcept -> NativeContext& = default;

auto NativeContext::vm() -> VM& { return *impl_->vm; }
auto NativeContext::vm() const -> VM const& { return *impl_->vm; }

auto NativeContext::argument_count() const -> std::size_t { return impl_->args.size(); }

auto NativeContext::argument(std::size_t index) const -> Value const&
{
    if (index >= impl_->args.size())
        error(std::format("missing argument {} ({} provided)", index, impl_->args.size()));
    return impl_->args[index];
}

auto NativeContext::self() const -> Value const& { return impl_->self; }
auto NativeContext::return_value() const -> Value const& { return impl_->result; }

auto NativeContext::set_return_value(Value value) -> void { impl_->result = std::move(value); }
auto NativeContext::return_undefined() -> void { impl_->result = Value::undefined(); }

auto NativeContext::function_space() const -> std::string_view { return impl_->space; }
auto NativeContext::function_name() const -> std::string_view { return impl_->name; }
auto NativeContext::thread_id() const -> std::uint64_t { return impl_->thread; }

[[noreturn]] auto NativeContext::error(std::string_view message) const -> void
{
    throw RuntimeError(std::string{ message }, impl_->module, impl_->function, impl_->offset, impl_->opcode, impl_->thread);
}

// ---------------------------------------------------------------------------
// VM::Impl
// ---------------------------------------------------------------------------
struct DebugRuntime
{
    bool enabled{};
    bool paused{};
    bool pause_requested{};
    bool stepping{};
    std::uint64_t paused_thread{};
    std::uint64_t step_thread{};
    std::vector<BreakpointInfo> breakpoints;
};

struct VM::Impl
{
    VM* owner{};
    VMConfig cfg;
    NativeRegistry registry;
    std::vector<std::shared_ptr<Module>> modules;
    std::unordered_map<std::string, std::size_t> aliases;
    std::string entry_module;

    std::vector<std::shared_ptr<Thread>> threads;
    std::unordered_map<std::uint64_t, std::size_t> thread_ids;
    std::uint64_t next_thread_id{ 1 };

    std::map<std::string, std::vector<std::uint8_t>> virtual_files;
    DebugRuntime debug;

    double clock{};
    double real_clock{};
    std::size_t executed{};

    ObjectPtr level;
    ObjectPtr game;
    ObjectPtr world;
    ObjectPtr classes;
    ObjectPtr anim;

    auto ensure_host() -> Host&
    {
        if (!cfg.host)
            cfg.host = std::make_shared<DefaultHost>();
        return *cfg.host;
    }

    auto find_module(std::string_view name) const -> std::shared_ptr<Module>
    {
        auto const key = normalize_path(name);
        if (auto const it = aliases.find(key); it != aliases.end())
            return modules[it->second];

        // A qualified path may be referenced by basename (and vice versa).
        auto found = std::size_t{ 0 };
        auto count = std::size_t{ 0 };
        for (auto const& [alias, index] : aliases)
        {
            if (alias == key || basename(alias) == key || basename(alias) == basename(key))
            {
                found = index;
                count++;
            }
        }
        if (count == 1)
            return modules[found];

        return nullptr;
    }

    auto register_module(std::shared_ptr<Module> module, std::string_view requested) -> void
    {
        auto const index = modules.size();
        modules.push_back(module);

        auto add_alias = [&](std::string_view name) {
            auto const key = normalize_path(name);
            if (!key.empty())
                aliases.try_emplace(key, index);
        };

        add_alias(module->name);
        add_alias(requested);
        add_alias(basename(module->name));
        add_alias(basename(requested));

        if (entry_module.empty())
            entry_module = normalize_path(module->name.empty() ? requested : module->name);
    }

    auto find_virtual_path(std::string_view requested, std::string_view extra_dir = {}) const -> std::optional<std::string>
    {
        auto const normalized = normalize_virtual_path(requested);
        if (normalized.empty())
            return std::nullopt;

        auto candidates = std::vector<std::string>{ normalized };
        auto add_candidate = [&](std::string_view base, std::string_view path) {
            auto candidate = normalize_virtual_path(path_join(normalize_virtual_path(base), path));
            if (!candidate.empty() && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(std::move(candidate));
        };

        if (!extra_dir.empty())
            add_candidate(extra_dir, normalized);

        for (auto const& path : cfg.search_paths)
            add_candidate(path.generic_string(), normalized);

        // Allow extensionless requests such as "dep_helper" to resolve a
        // virtual "scripts/dep_helper.gsc" file.
        auto expanded = candidates;
        for (auto const& candidate : candidates)
        {
            if (std::filesystem::path{ candidate }.has_extension())
                continue;
            for (auto const ext : { ".gsc", ".gsh", ".gscc", ".csc", ".bin" })
                expanded.push_back(candidate + ext);
        }

        for (auto const& candidate : expanded)
        {
            if (auto const it = virtual_files.find(candidate); it != virtual_files.end())
                return it->first;
        }

        auto wanted_names = std::vector<std::string>{ lower(normalized) };
        if (!std::filesystem::path{ normalized }.has_extension())
        {
            for (auto const ext : { ".gsc", ".gsh", ".gscc", ".csc", ".bin" })
                wanted_names.push_back(lower(normalized + ext));
        }

        auto match = std::optional<std::string>{};
        for (auto const& [key, value] : virtual_files)
        {
            (void)value;
            auto const key_lower = lower(key);
            auto const matches = std::any_of(wanted_names.begin(), wanted_names.end(), [&](std::string const& wanted) {
                return key_lower == wanted || basename(key_lower) == basename(wanted);
            });
            if (!matches)
                continue;
            if (match)
                return std::nullopt;
            match = key;
        }

        return match;
    }

    auto load_bytes(std::string requested, std::uint8_t const* data, std::size_t size, std::filesystem::path source_dir = {}) -> std::shared_ptr<Module>
    {
        if (data == nullptr || size == 0)
            throw Error(std::format("cannot load empty script '{}'", requested));

        auto compiled = std::vector<std::uint8_t>{};

        if (!detail::is_compiled(std::span<std::uint8_t const>{ data, size }))
        {
            auto options = detail::CompileOptions{};
            options.instance = cfg.instance;
            options.dev_blocks = cfg.execute_devblocks;
            options.search_paths = cfg.search_paths;

            if (source_dir.empty())
            {
                auto const parent = std::filesystem::path{ requested }.parent_path();
                if (!parent.empty())
                    source_dir = parent;
            }

            if (!source_dir.empty())
                options.search_paths.insert(options.search_paths.begin(), source_dir);

            auto const extra_dir = source_dir.generic_string();
            options.virtual_loader = [this, extra_dir](std::string const& include) -> std::vector<std::uint8_t> {
                auto const key = find_virtual_path(include, extra_dir);
                if (!key)
                    return {};
                auto const it = virtual_files.find(*key);
                if (it == virtual_files.end())
                    return {};
                return it->second;
            };

            compiled = detail::compile_source(requested, std::string_view{ reinterpret_cast<char const*>(data), size }, options);
            data = compiled.data();
            size = compiled.size();
        }

        auto module = detail::load_module(requested, data, size, detail::LoadOptions{ cfg.instance, cfg.t6_fixup });
        register_module(module, requested);
        return module;
    }

    auto load_from_file(std::string const& name) -> std::shared_ptr<Module>
    {
        auto candidates = std::vector<std::string>{ name };
        if (!name.ends_with(".gsc") && !name.ends_with(".gscc") && !name.ends_with(".csc"))
        {
            candidates.push_back(name + ".gsc");
            candidates.push_back(name + ".gscc");
            candidates.push_back(name + ".csc");
        }

        for (auto const& candidate : candidates)
        {
            auto const key = find_virtual_path(candidate);
            if (!key)
                continue;
            auto const it = virtual_files.find(*key);
            if (it == virtual_files.end() || it->second.empty())
                continue;
            auto const parent = parent_path(*key);
            return load_bytes(name, it->second.data(), it->second.size(), parent.empty() ? std::filesystem::path{} : std::filesystem::path{ parent });
        }

        auto const& host = ensure_host();

        if (auto bytes = host.load_script(name); bytes && !bytes->empty())
            return load_bytes(name, bytes->data(), bytes->size());

        for (auto const& path : cfg.search_paths)
        {
            for (auto const& candidate : candidates)
            {
                auto file = path / candidate;
                auto stream = std::ifstream{ file, std::ios::binary };
                if (!stream)
                    continue;

                stream.seekg(0, std::ios::end);
                auto const length = stream.tellg();
                if (length <= 0)
                    continue;
                stream.seekg(0, std::ios::beg);

                auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
                stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!stream)
                    continue;

                return load_bytes(name, bytes.data(), bytes.size(), file.parent_path());
            }
        }

        throw Error(std::format("could not load script module '{}'", name));
    }

    auto ensure_module(std::string_view name) -> std::shared_ptr<Module>
    {
        if (auto module = find_module(name))
            return module;
        return load_from_file(std::string{ name });
    }

    auto resolve_function(std::string_view module_name, std::string_view space, std::string_view name, bool allow_load) -> std::pair<std::shared_ptr<Module>, Function const*>
    {
        auto try_module = [&](std::shared_ptr<Module> const& module) -> Function const* {
            if (!module)
                return nullptr;
            if (auto fn = module->find_function(space, name); fn)
                return fn;
            if (!space.empty())
                return module->find_function({}, name);
            return nullptr;
        };

        if (!module_name.empty())
        {
            auto module = find_module(module_name);
            if (!module && allow_load)
            {
                try
                {
                    module = load_from_file(std::string{ module_name });
                }
                catch (...)
                {
                    module = nullptr;
                }
            }
            if (auto fn = try_module(module); fn)
                return { module, fn };
        }

        if (!space.empty())
        {
            if (auto module = find_module(space))
            {
                if (auto fn = try_module(module); fn)
                    return { module, fn };
            }
        }

        if (!entry_module.empty())
        {
            if (auto module = find_module(entry_module))
            {
                if (auto fn = try_module(module); fn)
                    return { module, fn };
            }
        }

        for (auto const& module : modules)
        {
            if (auto fn = try_module(module); fn)
                return { module, fn };
        }

        if (allow_load && !space.empty())
        {
            try
            {
                auto module = load_from_file(std::string{ space });
                if (auto fn = try_module(module); fn)
                    return { module, fn };
            }
            catch (...)
            {
            }
        }

        return {};
    }

    auto make_frame(std::shared_ptr<Module> module, Function const& function, std::span<Value const> args, Value self) -> Frame
    {
        auto frame = Frame{};
        frame.module = std::move(module);
        frame.function = &function;
        frame.self = std::move(self);
        frame.max_stack = cfg.max_stack_size;
        frame.locals.resize(function.params);

        auto const copied = std::min<std::size_t>(args.size(), function.params);
        for (std::size_t i = 0; i < copied; i++)
            frame.locals[i] = args[i];

        for (std::size_t i = function.params; i < args.size(); i++)
            frame.varargs.push_back(args[i]);

        return frame;
    }

    auto add_thread(std::shared_ptr<Module> module, Function const& function, std::span<Value const> args, Value self) -> ThreadRef
    {
        auto const live_threads = std::count_if(threads.begin(), threads.end(), [](auto const& thread) {
            return !terminal_status(thread->status);
        });

        if (cfg.max_threads != 0 && static_cast<std::size_t>(live_threads) >= cfg.max_threads)
            throw Error(std::format("thread limit of {} exceeded", cfg.max_threads));

        auto thread = std::make_shared<Thread>();
        thread->id = next_thread_id++;
        thread->self = self;
        thread->push_frame(make_frame(std::move(module), function, args, self));

        auto const index = threads.size();
        threads.push_back(thread);
        thread_ids.emplace(thread->id, index);

        return ThreadRef{ thread->id };
    }

    auto add_finished_thread(Value result) -> ThreadRef
    {
        auto thread = std::make_shared<Thread>();
        thread->id = next_thread_id++;
        thread->status = ThreadStatus::finished;
        thread->result = std::move(result);

        auto const index = threads.size();
        threads.push_back(thread);
        thread_ids.emplace(thread->id, index);

        return ThreadRef{ thread->id };
    }

    auto thread_by_id(std::uint64_t id) const -> std::shared_ptr<Thread>
    {
        auto const it = thread_ids.find(id);
        if (it == thread_ids.end())
            return nullptr;
        return threads[it->second];
    }

    [[noreturn]] auto fail(Thread const& thread, Frame const& frame, Instruction const& instruction, std::string_view message) const -> void
    {
        throw RuntimeError(std::string{ message },
            frame.module ? frame.module->name : std::string{},
            frame.function ? frame.function->name : std::string{},
            instruction.offset,
            instruction.name,
            thread.id);
    }

    auto return_from_frame(Thread& thread, Value value) -> void
    {
        if (thread.frames.empty())
            return;

        thread.frames.pop_back();

        if (thread.frames.empty())
        {
            thread.result = std::move(value);
            if (thread.status == ThreadStatus::running)
                thread.status = ThreadStatus::finished;
            return;
        }

        thread.frames.back().push(std::move(value));
    }

    auto invoke_native(Thread* thread, Frame* frame, Instruction const* instruction, Import const& import, std::vector<Value> args, Value self) -> Value
    {
        auto const* function = registry.find(import.space, import.name);

        if (!function && !import.space.empty())
            function = registry.find({}, import.name);

        if (!function)
            throw Error(std::format("unresolved native '{}'", import.space.empty() ? import.name : import.space + "::" + import.name));

        auto impl = std::make_unique<NativeContext::Impl>();
        impl->vm = owner;
        impl->thread = thread ? thread->id : 0;
        impl->module = frame && frame->module ? frame->module->name : std::string{};
        impl->function = frame && frame->function ? frame->function->name : std::string{};
        impl->space = import.space;
        impl->name = import.name;
        impl->offset = instruction ? instruction->offset : 0;
        impl->opcode = instruction ? instruction->name : std::string{};
        impl->args = std::move(args);
        impl->self = std::move(self);

        auto const module_name = impl->module;
        auto const function_name = impl->function;
        auto const offset = impl->offset;
        auto const opcode = impl->opcode;
        auto const thread_id = impl->thread;

        auto context = NativeContext{ std::move(impl) };

        try
        {
            (*function)(context);
        }
        catch (RuntimeError const&)
        {
            throw;
        }
        catch (std::exception const& e)
        {
            throw RuntimeError(e.what(), module_name, function_name, offset, opcode, thread_id);
        }

        return context.return_value();
    }

    auto execute(Thread& thread, Frame& frame, Instruction const& instruction) -> void;

    static auto terminal_status(ThreadStatus status) -> bool
    {
        return status == ThreadStatus::finished || status == ThreadStatus::failed || status == ThreadStatus::aborted;
    }

    auto has_live_threads() const -> bool
    {
        return std::any_of(threads.begin(), threads.end(), [](auto const& thread) { return !terminal_status(thread->status); });
    }

    auto has_paused_thread() const -> bool
    {
        return std::any_of(threads.begin(), threads.end(), [](auto const& thread) { return thread->status == ThreadStatus::paused; });
    }

    auto breakpoint_hit(Frame const& frame) const -> bool
    {
        if (!debug.enabled || debug.breakpoints.empty())
            return false;
        if (!frame.module || !frame.function)
            return false;

        return std::any_of(debug.breakpoints.begin(), debug.breakpoints.end(), [&](BreakpointInfo const& breakpoint) {
            return breakpoint.pc == frame.pc
                && breakpoint.function == frame.function->name
                && breakpoint.module == frame.module->name;
        });
    }

    auto pause_thread(Thread& thread) -> void
    {
        thread.status = ThreadStatus::paused;
        thread.yielded = false;
        debug.paused = true;
        debug.paused_thread = thread.id;
        debug.stepping = false;
    }

    auto next_ready_thread() -> std::shared_ptr<Thread>
    {
        for (auto const& thread : threads)
        {
            if (thread->status == ThreadStatus::ready && !thread->yielded)
                return thread;
        }

        auto next = std::shared_ptr<Thread>{};
        for (auto const& thread : threads)
        {
            if (thread->status != ThreadStatus::ready)
                continue;
            thread->yielded = false;
            if (!next)
                next = thread;
        }
        return next;
    }

    auto first_waiting_event() const -> std::shared_ptr<Thread>
    {
        for (auto const& thread : threads)
        {
            if (thread->status == ThreadStatus::waiting_event)
                return thread;
        }
        return nullptr;
    }

    [[noreturn]] auto fail_waiting(Thread const& thread) const -> void
    {
        auto module_name = std::string{};
        auto function_name = std::string{};
        auto offset = std::size_t{};
        auto opcode_name = std::string{};

        if (!thread.frames.empty())
        {
            auto const& frame = thread.frames.back();
            module_name = frame.module ? frame.module->name : std::string{};
            function_name = frame.function ? frame.function->name : std::string{};

            if (frame.function && frame.pc > 0 && frame.pc <= frame.function->instructions.size())
            {
                auto const& instruction = frame.function->instructions[frame.pc - 1];
                offset = instruction.offset;
                opcode_name = instruction.name;
            }
        }

        auto const message = std::format("deadlock: thread {} ('{}') is waiting for event '{}' which was never notified",
            thread.id, function_name.empty() ? "?" : function_name, thread.wait_event);

        throw RuntimeError(message, std::move(module_name), std::move(function_name), offset, std::move(opcode_name), thread.id);
    }

    auto tick_period() const -> double
    {
        if (!(cfg.tick_rate > 0.0) || !std::isfinite(cfg.tick_rate))
            throw Error("tick rate must be a positive finite number");

        return 1.0 / cfg.tick_rate;
    }

    auto advance_clock(double delta) -> void
    {
        if (!(delta > 0.0))
            return;

        if (cfg.real_time)
            std::this_thread::sleep_for(std::chrono::duration<double>{ delta });

        clock += delta;
        real_clock += delta;
    }

    auto wake_next_timed() -> bool
    {
        auto next = std::optional<double>{};

        for (auto const& thread : threads)
        {
            if (thread->status != ThreadStatus::waiting_time)
                continue;
            if (!next || thread->wake_time < *next)
                next = thread->wake_time;
        }

        if (!next)
            return false;

        if (*next > clock)
            advance_clock(*next - clock);

        wake_timed();
        return true;
    }

    auto run_scheduler(bool advance_time, std::uint64_t primary_id, bool fail_on_deadlock) -> std::size_t
    {
        auto const start = executed;
        auto budget = cfg.instruction_limit == 0 ? std::numeric_limits<std::size_t>::max() : cfg.instruction_limit;

        for (;;)
        {
            if (debug.paused)
                break;

            auto next = next_ready_thread();
            if (next)
            {
                run_thread(*next, budget);
                continue;
            }

            if (!has_live_threads())
                break;

            if (debug.paused || has_paused_thread())
                break;

            if (advance_time)
            {
                if (cfg.execution_mode == ExecutionMode::tick_based)
                {
                    auto const period = tick_period();
                    auto woke_frame = false;
                    for (auto const& thread : threads)
                    {
                        if (thread->status == ThreadStatus::waiting_frame)
                        {
                            thread->status = ThreadStatus::ready;
                            woke_frame = true;
                        }
                    }

                    if (woke_frame)
                    {
                        advance_clock(period);
                        wake_timed();
                        continue;
                    }

                    auto next = std::optional<double>{};
                    for (auto const& thread : threads)
                    {
                        if (thread->status != ThreadStatus::waiting_time)
                            continue;
                        if (!next || thread->wake_time < *next)
                            next = thread->wake_time;
                    }

                    if (next)
                    {
                        if (*next <= clock + time_epsilon)
                        {
                            wake_timed();
                            continue;
                        }

                        auto const delta = *next - clock;
                        auto const steps = std::max<std::uint64_t>(1,
                            static_cast<std::uint64_t>(std::ceil((delta - time_epsilon) / period)));
                        advance_clock(static_cast<double>(steps) * period);
                        wake_timed();
                        continue;
                    }
                }
                else
                {
                    auto woke_frame = false;
                    for (auto const& thread : threads)
                    {
                        if (thread->status == ThreadStatus::waiting_frame)
                        {
                            thread->status = ThreadStatus::ready;
                            woke_frame = true;
                        }
                    }

                    if (woke_frame)
                    {
                        advance_clock(cfg.frame_time);
                        continue;
                    }

                    if (wake_next_timed())
                        continue;
                }
            }

            if (fail_on_deadlock)
            {
                if (auto const waiter = first_waiting_event())
                    fail_waiting(*waiter);

                auto const primary = primary_id == 0 ? nullptr : thread_by_id(primary_id);
                throw RuntimeError("deadlock: no live thread can make progress", "", "", 0, "", primary ? primary->id : 0);
            }

            break;
        }

        return executed - start;
    }

    auto run_thread(Thread& thread, std::size_t& budget) -> void
    {
        thread.yielded = false;
        thread.status = ThreadStatus::running;

        while (thread.status == ThreadStatus::running)
        {
            if (thread.frames.empty())
            {
                thread.status = ThreadStatus::finished;
                return;
            }

            auto& frame = thread.frames.back();

            if (frame.pc >= frame.function->instructions.size())
            {
                return_from_frame(thread, Value::undefined());
                continue;
            }

            if (debug.enabled && debug.pause_requested)
            {
                debug.pause_requested = false;
                pause_thread(thread);
                return;
            }

            // Stepping deliberately ignores breakpoints on the instruction that
            // is about to execute; otherwise step would never get past one.
            auto const stepping_this_thread = debug.stepping && debug.step_thread == thread.id;
            if (!stepping_this_thread && breakpoint_hit(frame))
            {
                pause_thread(thread);
                return;
            }

            auto const& instruction = frame.function->instructions[frame.pc++];

            if (budget == 0)
            {
                auto const message = std::format("infinite loop detected: instruction limit of {} reached in thread {} ('{}')",
                    cfg.instruction_limit, thread.id, frame.function ? frame.function->name : "?");
                thread.status = ThreadStatus::failed;
                thread.error = message;
                fail(thread, frame, instruction, message);
            }

            try
            {
                execute(thread, frame, instruction);
            }
            catch (RuntimeError const& e)
            {
                thread.status = ThreadStatus::failed;
                thread.error = e.what();
                throw;
            }
            catch (Error const& e)
            {
                thread.status = ThreadStatus::failed;
                thread.error = e.what();
                fail(thread, frame, instruction, e.what());
            }
            catch (std::exception const& e)
            {
                thread.status = ThreadStatus::failed;
                thread.error = e.what();
                fail(thread, frame, instruction, e.what());
            }

            executed++;
            if (budget != std::numeric_limits<std::size_t>::max())
                budget--;

            if (debug.stepping && debug.step_thread == thread.id && thread.status == ThreadStatus::running)
            {
                pause_thread(thread);
                return;
            }
        }
    }

    auto run_ready(std::size_t limit) -> std::size_t
    {
        if (limit == 0)
            limit = std::numeric_limits<std::size_t>::max();

        auto const start = executed;

        for (;;)
        {
            if (debug.paused || has_paused_thread())
                break;

            auto next = next_ready_thread();
            if (!next)
                break;

            run_thread(*next, limit);
        }

        return executed - start;
    }

    auto wake_timed() -> void
    {
        for (auto& thread : threads)
        {
            if (thread->status == ThreadStatus::waiting_time && thread->wake_time <= clock + time_epsilon)
                thread->status = ThreadStatus::ready;
        }
    }

    auto resume_event(Thread& thread, std::span<Value const> args) -> void
    {
        for (auto const& arg : args)
            thread.delivered.push_back(arg);

        for (auto it = thread.delivered.rbegin(); it != thread.delivered.rend(); ++it)
            thread.current_frame().push(*it);
        thread.delivered.clear();
        thread.status = ThreadStatus::ready;
    }

    auto dispatch(Value const& target, std::string_view event, std::span<Value const> args) -> std::size_t
    {
        auto const name = lower(event);
        auto count = std::size_t{};

        // endon has priority over waittill: a thread subscribed to this event is
        // terminated even if it happens to be waiting on the same event.
        for (auto const& thread : threads)
        {
            if (terminal_status(thread->status))
                continue;

            for (auto const& subscription : thread->endons)
            {
                if (subscription.event == name && subscription.object.equals(target))
                {
                    thread->abort();
                    if (debug.paused_thread == thread->id)
                    {
                        debug.paused = false;
                        debug.paused_thread = 0;
                    }
                    count++;
                    break;
                }
            }
        }

        for (auto const& thread : threads)
        {
            if (thread->status != ThreadStatus::waiting_event)
                continue;

            auto const matches_event = thread->wait_match ? wildcard_match(thread->wait_event, name) : thread->wait_event == name;
            if (!matches_event || !thread->wait_object.equals(target))
                continue;

            if (args.size() < thread->wait_argc)
                continue;

            resume_event(*thread, args);
            count++;
        }

        return count;
    }

    auto ensure_globals() -> void
    {
        if (level && game && world && classes && anim)
            return;

        auto const& host = ensure_host();
        if (!level)
            level = host.level_object();
        if (!game)
            game = host.game_object();
        if (!world)
            world = host.world_object();
        if (!classes)
            classes = host.classes_object();
        if (!anim)
            anim = host.anim_object();

        if (!level)
            level = std::make_shared<ScriptObject>("level");
        if (!game)
            game = std::make_shared<ScriptObject>("game");
        if (!world)
            world = std::make_shared<ScriptObject>("world");
        if (!classes)
            classes = std::make_shared<ScriptObject>("classes");
        if (!anim)
            anim = std::make_shared<ScriptObject>("anim");
    }

    auto call_function(FunctionRef const& ref, std::span<Value const> args) -> Value
    {
        if (ref.kind == FunctionRef::Kind::native || (ref.kind == FunctionRef::Kind::unresolved && registry.find(ref.space, ref.name)))
        {
            auto import = Import{ .space = ref.space, .name = ref.name, .params = ref.params, .flags = ref.flags };
            return invoke_native(nullptr, nullptr, nullptr, import, std::vector<Value>{ args.begin(), args.end() }, Value::undefined());
        }

        auto [module, function] = resolve_function(ref.module, ref.space, ref.name, true);
        if (!function)
            throw Error(std::format("unresolved script function '{}'", ref.space.empty() ? ref.name : ref.space + "::" + ref.name));

        auto const id = add_thread(module, *function, args, Value::undefined());
        run_scheduler(true, id.id, true);

        auto thread = thread_by_id(id.id);
        if (!thread)
            throw Error(std::format("thread {} disappeared during execution", id.id));

        if (thread->status == ThreadStatus::failed)
            throw RuntimeError(thread->error.empty() ? "thread failed" : thread->error, "", "", 0, "", thread->id);

        return thread->result;
    }
};

// ---------------------------------------------------------------------------
// Instruction execution
// ---------------------------------------------------------------------------
auto VM::Impl::execute(Thread& thread, Frame& frame, Instruction const& instruction) -> void
{
    using xsk::arc::opcode;

    auto const depth = thread.frames.size() - 1;

    auto boolean = [](Value const& value) {
        return value.to_boolean();
    };

    auto local_create = [&](std::size_t index, Value value) {
        while (frame.locals.size() <= index)
        {
            frame.locals.push_back(Value::undefined());
            frame.local_names.emplace_back();
        }
        frame.locals[index] = std::move(value);
    };

    switch (instruction.opcode)
    {
        case opcode::OP_End:
            return_from_frame(thread, Value::undefined());
            return;

        case opcode::OP_Return:
            return_from_frame(thread, frame.pop_value());
            return;

        case opcode::OP_GetUndefined:
            frame.push(Value::undefined());
            return;

        case opcode::OP_GetZero:
            frame.push(Value::integer(0));
            return;

        case opcode::OP_GetByte:
        case opcode::OP_GetUnsignedShort:
            frame.push(Value::integer(parse_int(instruction.data.at(0))));
            return;

        case opcode::OP_GetInteger:
            if (instruction.data.size() >= 2)
                frame.push(Value::animtree(instruction.data[0]));
            else
                frame.push(Value::integer(parse_int(instruction.data.at(0))));
            return;

        case opcode::OP_GetNegByte:
        case opcode::OP_GetNegUnsignedShort:
            frame.push(Value::integer(-parse_int(instruction.data.at(0))));
            return;

        case opcode::OP_GetFloat:
            frame.push(Value::floating(parse_float(instruction.data.at(0))));
            return;

        case opcode::OP_GetString:
            frame.push(Value::string(instruction.data.at(0)));
            return;

        case opcode::OP_GetIString:
            frame.push(Value::istring(instruction.data.at(0)));
            return;

        case opcode::OP_GetHash:
            frame.push(Value::hash(instruction.data.at(0)));
            return;

        case opcode::OP_GetVector:
            frame.push(Value::vector(parse_float(instruction.data.at(0)), parse_float(instruction.data.at(1)), parse_float(instruction.data.at(2))));
            return;

        case opcode::OP_GetLevel:
        case opcode::OP_GetLevelObject:
            ensure_globals();
            frame.push(Value::object(level));
            return;

        case opcode::OP_GetGame:
        case opcode::OP_GetGameRef:
            ensure_globals();
            frame.push(Value::object(game));
            return;

        case opcode::OP_GetWorld:
        case opcode::OP_GetWorldObject:
            ensure_globals();
            frame.push(Value::object(world));
            return;

        case opcode::OP_GetClasses:
        case opcode::OP_GetClassesObject:
            ensure_globals();
            frame.push(Value::object(classes));
            return;

        case opcode::OP_GetAnim:
            ensure_globals();
            frame.push(Value::object(anim));
            return;

        case opcode::OP_GetAnimObject:
            ensure_globals();
            frame.push(Value::object(anim));
            return;

        case opcode::OP_GetAnimation:
            if (instruction.data.size() < 2)
                throw Error("OP_GetAnimation has a truncated operand");
            frame.push(Value::animation(instruction.data[0], instruction.data[1]));
            return;

        case opcode::OP_GetSelf:
        case opcode::OP_GetSelfObject:
            frame.push(frame.self);
            return;

        case opcode::OP_Object:
            frame.push(frame.self);
            return;

        case opcode::OP_ThreadObject:
            frame.push(Value::thread(ThreadRef{ thread.id }));
            return;

        case opcode::OP_Nop:
            return;

        case opcode::OP_Abort:
            thread.abort();
            return;

        case opcode::OP_New:
            throw Error("OP_New is not supported by this T6 interpreter");

        case opcode::OP_GetUintptr:
            frame.push(Value::integer(0));
            return;

        case opcode::OP_GetFunction:
        {
            auto import = parse_import(instruction);
            if (import.is_developer() && !cfg.execute_devblocks)
            {
                frame.push(Value::undefined());
                return;
            }

            auto ref = FunctionRef{};
            ref.space = import.space;
            ref.name = import.name;
            ref.params = import.params;
            ref.flags = import.flags;

            if (auto [module, function] = resolve_function({}, import.space, import.name, true); function)
            {
                ref.kind = FunctionRef::Kind::script;
                ref.module = module->name;
            }
            else if (registry.find(import.space, import.name) || registry.find({}, import.name))
            {
                ref.kind = FunctionRef::Kind::native;
            }

            frame.push(Value::function(std::move(ref)));
            return;
        }

        case opcode::OP_CreateLocalVariable:
        {
            auto const index = static_cast<std::size_t>(parse_int(instruction.data.at(0)));
            local_create(index, Value::undefined());
            if (frame.local_names.size() <= index)
                frame.local_names.resize(index + 1);
            frame.local_names[index] = instruction.data.size() > 1 ? instruction.data[1] : std::string{};
            return;
        }

        case opcode::OP_SafeCreateLocalVariables:
        {
            if (frame.locals_created)
                return;

            auto const& names = instruction.data;
            auto const param_count = static_cast<std::size_t>(frame.function->params);

            if (frame.locals.size() < param_count)
                frame.locals.resize(param_count);

            frame.local_names.resize(frame.locals.size());

            for (std::size_t i = 0; i < names.size(); i++)
            {
                if (i < frame.locals.size())
                {
                    frame.local_names[i] = names[i];
                    continue;
                }

                if (names[i] == "vararg" && !frame.varargs.empty())
                {
                    auto array = std::make_shared<Array>();
                    for (std::size_t j = 0; j < frame.varargs.size(); j++)
                        array->set(Value::integer(static_cast<std::int32_t>(j)), frame.varargs[j]);

                    frame.locals.push_back(Value::array(std::move(array)));
                }
                else
                {
                    frame.locals.push_back(Value::undefined());
                }

                frame.local_names.push_back(names[i]);
            }

            frame.locals_created = true;
            return;
        }

        case opcode::OP_RemoveLocalVariables:
        {
            auto const count = instruction.data.empty() ? frame.locals.size() : parse_size(instruction.data[0]);
            auto const removable = frame.locals.size() > frame.function->params ? frame.locals.size() - frame.function->params : 0;
            auto const remove = std::min(count, removable);
            frame.locals.resize(frame.locals.size() - remove);
            frame.local_names.resize(frame.locals.size());
            return;
        }

        case opcode::OP_EvalLocalVariableCached:
        case opcode::OP_EvalLocalVariableCachedDebug:
        {
            auto const index = static_cast<std::size_t>(parse_int(instruction.data.at(0)));
            frame.push(frame.local(index));
            return;
        }

        case opcode::OP_EvalLocalVariableRefCached:
        case opcode::OP_EvalLocalVariableRefCachedDebug:
        {
            auto const index = static_cast<std::size_t>(parse_int(instruction.data.at(0)));
            (void)frame.local(index);
            frame.push(StackItem::make_reference(make_local_ref(depth, index)));
            return;
        }

        case opcode::OP_EvalArray:
        {
            auto const object = frame.pop_operand(thread);
            auto const key = frame.pop_value();
            if (object.is_array())
                frame.push(object.as_array().get(key));
            else if (object.is_object())
                frame.push(object.as_object().get_index(key));
            else if (object.is_vector())
            {
                auto const index = key.to_integer();
                auto const vector = object.as_vector();
                if (index == 0) frame.push(Value::floating(vector.x));
                else if (index == 1) frame.push(Value::floating(vector.y));
                else if (index == 2) frame.push(Value::floating(vector.z));
                else frame.push(Value::undefined());
            }
            else
                throw Error(std::format("cannot index into {}", object.type_name()));
            return;
        }

        case opcode::OP_EvalArrayRef:
        {
            auto const container = frame.pop_operand(thread);
            auto const key = frame.pop_value();
            frame.push(StackItem::make_reference(make_array_element_ref(container, key)));
            return;
        }

        case opcode::OP_EvalLocalArrayRefCached:
        {
            auto const key = frame.pop_value();
            auto const index = static_cast<std::size_t>(parse_int(instruction.data.at(0)));
            auto const object = frame.local(index);
            frame.push(StackItem::make_reference(make_array_element_ref(object, key)));
            return;
        }

        case opcode::OP_ClearArray:
        {
            auto const reference = frame.pop_reference();
            auto const key = frame.pop_value();
            auto container = reference->read(thread);
            if (reference->kind == LValue::Kind::local)
            {
                auto const local_index = reference->local_index;
                auto value = frame.local(local_index);
                if (value.is_array())
                    value.as_array().erase(key);
                else if (value.is_object())
                    value.as_object().clear_index(key);
                else
                    throw Error("cannot clear an element of a non-container");
                return;
            }

            if (container.is_array())
                container.as_array().erase(key);
            else if (container.is_object())
                container.as_object().clear_index(key);
            else
                throw Error("cannot clear an element of a non-container");
            return;
        }

        case opcode::OP_EmptyArray:
            frame.push(Value::array());
            return;

        case opcode::OP_EvalFieldVariable:

        case opcode::OP_SelfEvalFieldVariable:
        {
            auto const field = instruction.data.at(0);
            auto const object = frame.pop_value();

            if (object.is_array())
            {
                if (field == "size")
                    frame.push(Value::integer(static_cast<std::int32_t>(object.as_array().size())));
                else
                    throw Error(std::format("arrays only support indexing and the .size property; cannot read field '{}'", field));
                return;
            }

            if (object.is_object())
                frame.push(object.as_object().get_field(field));
            else
                throw Error(std::format("cannot read field '{}' from {}", field, object.type_name()));
            return;
        }

        case opcode::OP_EvalFieldVariableRef:
        case opcode::OP_LevelEvalFieldVariableRef:
        case opcode::OP_SelfEvalFieldVariableRef:
        {
            auto const field = instruction.data.at(0);
            auto const object = frame.pop_value();

            if (object.is_array())
            {
                if (field == "size")
                    throw Error("array .size is read-only");
                throw Error(std::format("arrays only support indexing and the .size property; cannot access field '{}'", field));
            }

            frame.push(StackItem::make_reference(make_field_ref(object, field)));
            return;
        }

        case opcode::OP_ClearFieldVariable:
        {
            auto const field = instruction.data.at(0);
            auto const object = frame.pop_value();

            if (object.is_array())
                throw Error(std::format("arrays only support indexing and the .size property; cannot clear field '{}'", field));

            if (!object.is_object())
                throw Error(std::format("cannot clear field '{}' on {}", field, object.type_name()));
            object.as_object().clear_field(field);
            return;
        }

        case opcode::OP_SafeSetVariableFieldCached:
            throw Error("OP_SafeSetVariableFieldCached is not supported by this T6 interpreter");

        case opcode::OP_SafeSetWaittillVariableFieldCached:
        {
            auto const index = static_cast<std::size_t>(parse_int(instruction.data.at(0)));
            auto const value = frame.pop_value();
            frame.local(index) = value;
            return;
        }

        case opcode::OP_ClearParams:
            frame.stack.clear();
            return;

        case opcode::OP_CheckClearParams:
            return;

        case opcode::OP_SetVariableField:
        {
            auto const reference = frame.pop_reference();
            auto const value = frame.pop_value();
            reference->write(thread, value);
            return;
        }

        case opcode::OP_CastFieldObject:
        {
            auto const value = frame.pop_value();
            frame.push(value);
            return;
        }

        case opcode::OP_CastBool:
            frame.push(Value::integer(boolean(frame.pop_value()) ? 1 : 0));
            return;

        case opcode::OP_BoolNot:
            frame.push(Value::integer(boolean(frame.pop_value()) ? 0 : 1));
            return;

        case opcode::OP_BoolComplement:
            frame.push(Value::integer(~frame.pop_value().to_integer()));
            return;

        case opcode::OP_Inc:
        case opcode::OP_Dec:
        {
            auto const reference = frame.pop_reference();
            auto value = reference->read(thread);
            auto const delta = instruction.opcode == opcode::OP_Inc ? 1 : -1;

            if (value.is_integer())
                value = Value::integer(value.as_integer() + delta);
            else if (value.is_floating())
                value = Value::floating(value.as_floating() + static_cast<float>(delta));
            else
                value = Value::integer(value.to_integer() + delta);

            reference->write(thread, value);
            return;
        }

        case opcode::OP_JumpOnFalse:
        case opcode::OP_JumpOnTrue:
        {
            auto const value = frame.pop_value();
            auto const jump = instruction.opcode == opcode::OP_JumpOnFalse ? !boolean(value) : boolean(value);
            if (jump)
                frame.pc = instruction.jump_target;
            return;
        }

        case opcode::OP_JumpOnFalseExpr:
        case opcode::OP_JumpOnTrueExpr:
        {
            auto const value = frame.pop_value();
            auto const jump = instruction.opcode == opcode::OP_JumpOnFalseExpr ? !boolean(value) : boolean(value);
            if (jump)
            {
                frame.push(value);
                frame.pc = instruction.jump_target;
            }
            return;
        }

        case opcode::OP_Jump:
        case opcode::OP_JumpBack:
        case opcode::OP_DevblockBegin:
        {
            if (instruction.opcode == opcode::OP_DevblockBegin && cfg.execute_devblocks)
                return;
            frame.pc = instruction.jump_target;
            return;
        }

        case opcode::OP_DevblockEnd:
            return;

        case opcode::OP_Switch:
        {
            auto const value = frame.pop_value();
            auto const* selected = static_cast<SwitchEntry const*>(nullptr);
            auto const* fallback = static_cast<SwitchEntry const*>(nullptr);

            for (auto const& entry : instruction.switch_entries)
            {
                if (entry.is_default)
                {
                    fallback = &entry;
                    continue;
                }

                if (entry.is_string)
                {
                    if (is_string_like(value) && value.as_string() == entry.string)
                        selected = &entry;
                }
                else if (is_numeric(value) && value.to_integer() == entry.integer)
                {
                    selected = &entry;
                }

                if (selected)
                    break;
            }

            if (!selected)
                selected = fallback;

            if (selected)
                frame.pc = selected->target_index;
            else
                frame.pc = instruction.switch_target + 1;
            return;
        }

        case opcode::OP_EndSwitch:
            return;

        case opcode::OP_Vector:
        {
            auto const x = frame.pop_value();
            auto const y = frame.pop_value();
            auto const z = frame.pop_value();
            frame.push(Value::vector(static_cast<float>(as_double(x)), static_cast<float>(as_double(y)), static_cast<float>(as_double(z))));
            return;
        }

        case opcode::OP_VectorConstant:
        {
            auto const flags = parse_int(instruction.data.at(0));
            auto const x = (flags & 0x20) ? 1.0f : ((flags & 0x10) ? -1.0f : 0.0f);
            auto const y = (flags & 0x08) ? 1.0f : ((flags & 0x04) ? -1.0f : 0.0f);
            auto const z = (flags & 0x02) ? 1.0f : ((flags & 0x01) ? -1.0f : 0.0f);
            frame.push(Value::vector(x, y, z));
            return;
        }

        case opcode::OP_IsDefined:
            frame.push(Value::integer(frame.pop_value().is_undefined() ? 0 : 1));
            return;

        case opcode::OP_VectorScale:
        {
            auto const vector = frame.pop_value();
            auto const scale = frame.pop_value();
            auto const v = vector_of(vector);
            auto const s = static_cast<float>(as_double(scale));
            frame.push(Value::vector(v.x * s, v.y * s, v.z * s));
            return;
        }

        case opcode::OP_AnglesToUp:
            frame.push(Value::vector(angles_to_up(vector_of(frame.pop_value()))));
            return;

        case opcode::OP_AnglesToRight:
            frame.push(Value::vector(angles_to_right(vector_of(frame.pop_value()))));
            return;

        case opcode::OP_AnglesToForward:
            frame.push(Value::vector(angles_to_forward(vector_of(frame.pop_value()))));
            return;

        case opcode::OP_AngleClamp180:
        {
            auto value = frame.pop_value();
            if (value.is_vector())
            {
                auto const v = value.as_vector();
                frame.push(Value::vector(clamp_angle_180(v.x), clamp_angle_180(v.y), clamp_angle_180(v.z)));
            }
            else
            {
                frame.push(Value::floating(clamp_angle_180(static_cast<float>(as_double(value)))));
            }
            return;
        }

        case opcode::OP_VectorToAngles:
            frame.push(Value::vector(vector_to_angles(vector_of(frame.pop_value()))));
            return;

        case opcode::OP_Abs:
        {
            auto value = frame.pop_value();
            if (value.is_vector())
            {
                auto const v = value.as_vector();
                frame.push(Value::vector(std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)));
            }
            else if (value.is_integer())
                frame.push(Value::integer(std::abs(value.as_integer())));
            else
                frame.push(Value::floating(std::fabs(value.to_floating())));
            return;
        }

        case opcode::OP_GetTime:
            frame.push(Value::floating(static_cast<float>(clock)));
            return;

        case opcode::OP_RealWait:
        case opcode::OP_Wait:
        {
            auto const duration = static_cast<double>(as_double(frame.pop_value()));
            if (!std::isfinite(duration))
                throw Error("wait duration must be a finite number of seconds");

            if (duration <= 0.0)
            {
                thread.status = ThreadStatus::waiting_frame;
            }
            else
            {
                thread.wake_time = clock + duration;
                thread.status = ThreadStatus::waiting_time;
                thread.wait_event.clear();
            }
            return;
        }

        case opcode::OP_WaitTillFrameEnd:
            thread.status = ThreadStatus::waiting_frame;
            return;

        case opcode::OP_Notify:
        {
            auto const object = frame.pop_value();
            auto const event = frame.pop_value();

            auto args = std::vector<Value>{};
            while (!frame.stack.empty() && frame.peek_item().kind != StackItem::Kind::marker)
                args.push_back(frame.pop_value());
            frame.expect_marker();

            dispatch(object, event.as_string(), args);
            return;
        }

        case opcode::OP_EndOn:
        {
            auto const object = frame.pop_value();
            auto const event = frame.pop_value();
            thread.endons.push_back(EndOnSubscription{ object, lower(event.as_string()) });

            auto const has_ready_sibling = std::any_of(threads.begin(), threads.end(), [&](auto const& other) {
                return other.get() != &thread && other->status == ThreadStatus::ready;
            });
            if (has_ready_sibling)
            {
                thread.yielded = true;
                thread.status = ThreadStatus::ready;
            }
            return;
        }

        case opcode::OP_WaitTill:
        case opcode::OP_WaitTillMatch:
        {
            auto const object = frame.pop_value();
            auto const event = frame.pop_value();

            thread.wait_event = lower(event.as_string());
            thread.wait_object = object;
            thread.wait_match = instruction.opcode == opcode::OP_WaitTillMatch;
            thread.wait_argc = 0;

            if (thread.wait_match)
            {
                // Match arguments are compile-time patterns. The VM currently
                // uses them as a minimum argument count; see docs/opcodes.md.
                thread.wait_argc = instruction.data.empty() ? 0 : parse_size(instruction.data[0]);
                for (std::size_t i = 0; i < thread.wait_argc; i++)
                    (void)frame.pop_value();
            }

            thread.status = ThreadStatus::waiting_event;
            return;
        }

        case opcode::OP_PreScriptCall:
        case opcode::OP_VoidCodePos:
            frame.push(StackItem::make_marker());
            return;

        case opcode::OP_ScriptFunctionCall:
        case opcode::OP_ScriptMethodCall:
        case opcode::OP_ScriptFunctionCallClass:
        case opcode::OP_ScriptThreadCall:
        case opcode::OP_ScriptMethodThreadCall:
        case opcode::OP_ScriptThreadCallClass:
        {
            auto const import = parse_import(instruction);
            auto self = frame.self;
            if (import.is_method())
                self = frame.pop_value();

            auto args = std::vector<Value>{};
            args.reserve(import.params);
            for (std::size_t i = 0; i < import.params; i++)
                args.push_back(frame.pop_value());
            frame.expect_marker();

            if (import.is_developer() && !cfg.execute_devblocks)
            {
                if (!import.is_thread())
                    frame.push(Value::undefined());
                return;
            }

            auto [module, function] = resolve_function({}, import.space, import.name, true);

            if (!function)
            {
                if (registry.find(import.space, import.name) || registry.find({}, import.name))
                {
                    auto result = invoke_native(&thread, &frame, &instruction, import, std::move(args), std::move(self));
                    if (import.is_thread())
                        frame.push(Value::thread(add_finished_thread(std::move(result))));
                    else
                        frame.push(std::move(result));
                    return;
                }

                throw Error(std::format("unresolved script function '{}'", import.space.empty() ? import.name : import.space + "::" + import.name));
            }

            if (import.is_thread())
            {
                auto const id = add_thread(module, *function, args, self);
                frame.push(Value::thread(id));
            }
            else
            {
                if (cfg.max_call_depth != 0 && thread.frames.size() >= cfg.max_call_depth)
                    throw Error(std::format("maximum recursion depth of {} exceeded while calling '{}'", cfg.max_call_depth, function->name));
                thread.push_frame(make_frame(module, *function, args, self));
                return;
            }
            return;
        }

        case opcode::OP_ScriptFunctionCallPointer:
        case opcode::OP_ScriptMethodCallPointer:
        case opcode::OP_ScriptThreadCallPointer:
        case opcode::OP_ScriptMethodThreadCallPointer:
        {
            auto const arg_count = static_cast<std::size_t>(parse_import(instruction, true).params);
            auto const target = frame.pop_value();

            auto self = frame.self;
            if (instruction.opcode == opcode::OP_ScriptMethodCallPointer || instruction.opcode == opcode::OP_ScriptMethodThreadCallPointer)
                self = frame.pop_value();

            auto args = std::vector<Value>{};
            args.reserve(arg_count);
            for (std::size_t i = 0; i < arg_count; i++)
                args.push_back(frame.pop_value());
            frame.expect_marker();

            if (!target.is_function())
                throw Error(std::format("cannot call {}", target.type_name()));

            auto const& ref = target.as_function();
            auto [module, function] = resolve_function(ref.module, ref.space, ref.name, true);

            if (!function)
            {
                auto const import = Import{ .space = ref.space, .name = ref.name, .params = static_cast<std::uint8_t>(arg_count), .flags = ref.flags };
                if (registry.find(ref.space, ref.name) || registry.find({}, ref.name))
                {
                    auto result = invoke_native(&thread, &frame, &instruction, import, std::move(args), std::move(self));
                    if (instruction.opcode == opcode::OP_ScriptThreadCallPointer || instruction.opcode == opcode::OP_ScriptMethodThreadCallPointer)
                        frame.push(Value::thread(add_finished_thread(std::move(result))));
                    else
                        frame.push(std::move(result));
                    return;
                }

                throw Error(std::format("unresolved script function '{}'", ref.space.empty() ? ref.name : ref.space + "::" + ref.name));
            }

            if (instruction.opcode == opcode::OP_ScriptThreadCallPointer || instruction.opcode == opcode::OP_ScriptMethodThreadCallPointer)
            {
                auto const id = add_thread(module, *function, args, self);
                frame.push(Value::thread(id));
            }
            else
            {
                if (cfg.max_call_depth != 0 && thread.frames.size() >= cfg.max_call_depth)
                    throw Error(std::format("maximum recursion depth of {} exceeded while calling '{}'", cfg.max_call_depth, function->name));
                thread.push_frame(make_frame(module, *function, args, self));
                return;
            }
            return;
        }

        case opcode::OP_CallBuiltin:
        case opcode::OP_CallBuiltinMethod:
        {
            auto const import = parse_import(instruction);
            auto self = frame.self;
            if (import.is_method())
                self = frame.pop_value();

            auto args = std::vector<Value>{};
            args.reserve(import.params);
            for (std::size_t i = 0; i < import.params; i++)
                args.push_back(frame.pop_value());
            frame.expect_marker();

            if (import.is_developer() && !cfg.execute_devblocks)
            {
                frame.push(Value::undefined());
                return;
            }

            frame.push(invoke_native(&thread, &frame, &instruction, import, std::move(args), std::move(self)));
            return;
        }

        case opcode::OP_DecTop:
            (void)frame.pop_item();
            return;

        case opcode::OP_SafeDecTop:
            if (!frame.stack.empty())
                frame.stack.pop_back();
            return;

        case opcode::OP_GetDvar:
        case opcode::OP_GetDvarInt:
        case opcode::OP_GetDvarFloat:
        case opcode::OP_GetDvarVector:
        case opcode::OP_GetDvarColorRed:
        case opcode::OP_GetDvarColorGreen:
        case opcode::OP_GetDvarColorBlue:
        case opcode::OP_GetDvarColorAlpha:
        {
            auto const name = frame.pop_value().to_display_string();
            auto const& host = ensure_host();
            if (instruction.opcode == opcode::OP_GetDvar)
                frame.push(host.dvar(name));
            else if (instruction.opcode == opcode::OP_GetDvarInt)
                frame.push(Value::integer(host.dvar_int(name)));
            else if (instruction.opcode == opcode::OP_GetDvarFloat)
                frame.push(Value::floating(host.dvar_float(name)));
            else if (instruction.opcode == opcode::OP_GetDvarVector)
                frame.push(Value::vector(host.dvar_vector(name)));
            else
            {
                auto const color = host.dvar_vector(name);
                if (instruction.opcode == opcode::OP_GetDvarColorRed)
                    frame.push(Value::floating(color.x));
                else if (instruction.opcode == opcode::OP_GetDvarColorGreen)
                    frame.push(Value::floating(color.y));
                else if (instruction.opcode == opcode::OP_GetDvarColorBlue)
                    frame.push(Value::floating(color.z));
                else
                    frame.push(Value::floating(1.0f));
            }
            return;
        }

        case opcode::OP_FirstArrayKey:
        {
            auto const object = frame.pop_value();
            if (object.is_array())
                frame.push(object.as_array().first_key());
            else if (object.is_object())
                frame.push(object.as_object().first_key());
            else
                throw Error(std::format("cannot iterate {}", object.type_name()));
            return;
        }

        case opcode::OP_NextArrayKey:
        {
            auto const object = frame.pop_value();
            auto const key = frame.pop_value();
            if (object.is_array())
                frame.push(object.as_array().next_key(key));
            else if (object.is_object())
                frame.push(object.as_object().next_key(key));
            else
                throw Error(std::format("cannot iterate {}", object.type_name()));
            return;
        }

        case opcode::OP_SizeOf:
        {
            auto const object = frame.pop_value();
            if (object.is_array())
                frame.push(Value::integer(static_cast<std::int32_t>(object.as_array().size())));
            else if (object.is_object())
                frame.push(Value::integer(static_cast<std::int32_t>(object.as_object().size())));
            else if (is_string_like(object))
                frame.push(Value::integer(static_cast<std::int32_t>(object.as_string().size())));
            else
                frame.push(Value::integer(0));
            return;
        }

        case opcode::OP_Plus:
        case opcode::OP_Minus:
        case opcode::OP_Multiply:
        case opcode::OP_Divide:
        case opcode::OP_Modulus:
        case opcode::OP_Bit_Or:
        case opcode::OP_Bit_Xor:
        case opcode::OP_Bit_And:
        case opcode::OP_ShiftLeft:
        case opcode::OP_ShiftRight:
        case opcode::OP_Equal:
        case opcode::OP_NotEqual:
        case opcode::OP_SuperEqual:
        case opcode::OP_SuperNotEqual:
        case opcode::OP_LessThan:
        case opcode::OP_GreaterThan:
        case opcode::OP_LessThanOrEqualTo:
        case opcode::OP_GreaterThanOrEqualTo:
        {
            auto const rhs = frame.pop_value();
            auto const lhs = frame.pop_value();
            frame.push(apply_binary(instruction.opcode, lhs, rhs));
            return;
        }

        case opcode::OP_GetAPIFunction:
        {
            auto const import = parse_import(instruction);
            auto ref = FunctionRef{};
            ref.kind = registry.find(import.space, import.name) || registry.find({}, import.name) ? FunctionRef::Kind::native : FunctionRef::Kind::unresolved;
            ref.space = import.space;
            ref.name = import.name;
            ref.params = import.params;
            ref.flags = import.flags;
            frame.push(Value::function(std::move(ref)));
            return;
        }

        case opcode::OP_ProfileStart:
        case opcode::OP_ProfileStop:
            return;

        case opcode::OP_EvalLocalVariable:
        case opcode::OP_EvalLocalVariableRef:
            throw Error(std::format("{} is not supported by this T6 interpreter", instruction.name));

        case opcode::OP_Invalid:
        case opcode::OP_Count:
            throw Error(std::format("invalid opcode {}", instruction.name));

        default:
            throw Error(std::format("unimplemented opcode {} (0x{:02X}) at 0x{:X}", instruction.name, static_cast<std::uint8_t>(instruction.opcode), instruction.offset));
    }
}

// ---------------------------------------------------------------------------
// Public VM API
// ---------------------------------------------------------------------------
VM::VM(VMConfig config) : impl_(std::make_unique<Impl>())
{
    impl_->owner = this;
    impl_->cfg = std::move(config);
    impl_->ensure_host();

    if (impl_->cfg.execution_mode == ExecutionMode::tick_based &&
        (!(impl_->cfg.tick_rate > 0.0) || !std::isfinite(impl_->cfg.tick_rate)))
    {
        throw Error("tick rate must be a positive finite number");
    }

    impl_->registry.register_function("", "spawnstruct", [](NativeContext& context) {
        context.set_return_value(Value::object(std::make_shared<ScriptObject>("struct")));
    });

    impl_->registry.register_function("", "array", [](NativeContext& context) {
        auto result = Value::array();
        auto& array = result.as_array();
        for (std::size_t i = 0; i < context.argument_count(); i++)
            array.set(Value::integer(static_cast<std::int32_t>(i)), context.argument(i));
        context.set_return_value(std::move(result));
    });

    auto register_predicate = [this](std::string_view name, auto predicate) {
        impl_->registry.register_function("", name, [predicate](NativeContext& context) {
            auto const matches = context.argument_count() != 0 && predicate(context.argument(0));
            context.set_return_value(Value::integer(matches ? 1 : 0));
        });
    };

    register_predicate("isarray", [](Value const& value) { return value.is_array(); });
    register_predicate("isstring", [](Value const& value) { return value.is_string() || value.is_istring(); });
    register_predicate("isint", [](Value const& value) { return value.is_integer() || value.is_boolean(); });
    register_predicate("isfloat", [](Value const& value) { return value.is_floating(); });
}

VM::~VM() = default;
VM::VM(VM&&) noexcept = default;
auto VM::operator=(VM&&) noexcept -> VM& = default;

auto VM::config() const -> VMConfig const& { return impl_->cfg; }
auto VM::natives() -> NativeRegistry& { return impl_->registry; }
auto VM::natives() const -> NativeRegistry const& { return impl_->registry; }

auto VM::host() -> Host& { return impl_->ensure_host(); }
auto VM::host() const -> Host const& { return impl_->ensure_host(); }

auto VM::set_host(std::shared_ptr<Host> host) -> void
{
    impl_->cfg.host = std::move(host);
    impl_->ensure_host();

    // Re-query singleton objects on next use.
    impl_->level.reset();
    impl_->game.reset();
    impl_->world.reset();
    impl_->classes.reset();
    impl_->anim.reset();
}

auto VM::add_search_path(std::filesystem::path path) -> void { impl_->cfg.search_paths.push_back(std::move(path)); }
auto VM::search_paths() const -> std::vector<std::filesystem::path> const& { return impl_->cfg.search_paths; }

auto VM::load_module(std::string name, std::span<std::uint8_t const> data) -> ModuleInfo
{
    if (auto existing = impl_->find_module(name))
        return module_info(existing->name);

    auto module = impl_->load_bytes(std::move(name), data.data(), data.size());
    return module_info(module->name);
}

auto VM::load_module(std::string name, std::vector<std::uint8_t> data) -> ModuleInfo
{
    return load_module(std::move(name), std::span<std::uint8_t const>{ data.data(), data.size() });
}

auto VM::load_module_file(std::filesystem::path const& path, std::optional<std::string> name) -> ModuleInfo
{
    if (auto const key = impl_->find_virtual_path(path.generic_string()))
    {
        auto const it = impl_->virtual_files.find(*key);
        if (it != impl_->virtual_files.end() && !it->second.empty())
        {
            auto const parent = parent_path(*key);
            if (!parent.empty() && std::find(impl_->cfg.search_paths.begin(), impl_->cfg.search_paths.end(), parent) == impl_->cfg.search_paths.end())
                impl_->cfg.search_paths.push_back(parent);
            auto module_name = name.value_or(std::filesystem::path{ *key }.stem().string());
            auto module = impl_->load_bytes(module_name, it->second.data(), it->second.size(), parent.empty() ? std::filesystem::path{} : std::filesystem::path{ parent });
            return module_info(module->name);
        }
    }

    auto stream = std::ifstream{ path, std::ios::binary };
    if (!stream)
        throw Error(std::format("could not open '{}'", path.string()));

    stream.seekg(0, std::ios::end);
    auto const length = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (length <= 0)
        throw Error(std::format("script '{}' is empty", path.string()));

    auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream)
        throw Error(std::format("could not read '{}'", path.string()));

    auto const parent = path.parent_path();
    if (!parent.empty() && std::find(impl_->cfg.search_paths.begin(), impl_->cfg.search_paths.end(), parent) == impl_->cfg.search_paths.end())
        impl_->cfg.search_paths.push_back(parent);

    auto module_name = name.value_or(path.stem().string());
    auto module = impl_->load_bytes(module_name, bytes.data(), bytes.size(), parent);
    return module_info(module->name);
}

auto VM::load_dependencies(std::string_view module_name) -> void
{
    auto module = impl_->find_module(module_name);
    if (!module)
        throw Error(std::format("module '{}' is not loaded", module_name));

    for (auto const& include : module->includes)
    {
        if (impl_->find_module(include))
            continue;
        impl_->ensure_module(include);
    }
}

auto VM::has_module(std::string_view name) const -> bool { return impl_->find_module(name) != nullptr; }

auto VM::module_info(std::string_view name) const -> ModuleInfo const&
{
    auto module = impl_->find_module(name);
    if (!module)
        throw Error(std::format("module '{}' is not loaded", name));

    if (!module->info)
    {
        auto info = ModuleInfo{};
        info.name = module->name;
        info.includes = module->includes;
        info.functions.reserve(module->functions.size());
        for (auto const& function : module->functions)
        {
            info.functions.push_back(FunctionInfo{
                .name = function.name,
                .space = function.space,
                .params = function.params,
                .flags = function.flags,
                .offset = function.offset,
                .size = function.size,
            });
        }
        module->info = std::move(info);
    }

    return *module->info;
}

auto VM::modules() const -> std::vector<ModuleInfo>
{
    auto result = std::vector<ModuleInfo>{};
    result.reserve(impl_->modules.size());
    for (auto const& module : impl_->modules)
        result.push_back(module_info(module->name));
    return result;
}

auto VM::disassembly(std::string_view name) const -> std::string const&
{
    auto module = impl_->find_module(name);
    if (!module)
        throw Error(std::format("module '{}' is not loaded", name));
    return module->disassembly;
}

auto VM::call(std::string_view function, std::span<Value const> args) -> Value
{
    if (impl_->entry_module.empty())
        throw Error("no module loaded");

    auto space = std::string{};
    auto name = std::string{ function };
    if (auto const pos = name.rfind("::"); pos != std::string::npos)
    {
        space = name.substr(0, pos);
        name = name.substr(pos + 2);
    }

    auto [module, resolved] = impl_->resolve_function(impl_->entry_module, space, name, true);
    if (!resolved)
        throw Error(std::format("unresolved script function '{}'", function));

    auto ref = FunctionRef{};
    ref.kind = FunctionRef::Kind::script;
    ref.module = module->name;
    ref.space = space;
    ref.name = name;
    return call(ref, args);
}

auto VM::call(std::string_view module_name, std::string_view function, std::span<Value const> args) -> Value
{
    auto ref = FunctionRef{};
    ref.kind = FunctionRef::Kind::script;
    ref.module = std::string{ module_name };
    ref.name = std::string{ function };

    if (auto const pos = ref.name.rfind("::"); pos != std::string::npos)
    {
        ref.space = ref.name.substr(0, pos);
        ref.name = ref.name.substr(pos + 2);
    }

    return call(ref, args);
}

auto VM::call(FunctionRef const& function, std::span<Value const> args) -> Value
{
    return impl_->call_function(function, args);
}

auto VM::start(std::string_view module_name, std::string_view function, std::span<Value const> args) -> ThreadRef
{
    auto [module, resolved] = impl_->resolve_function(module_name, {}, function, true);
    if (!resolved)
        throw Error(std::format("unresolved script function '{}'", function));
    return impl_->add_thread(module, *resolved, args, Value::undefined());
}

auto VM::start(FunctionRef const& function, std::span<Value const> args) -> ThreadRef
{
    auto [module, resolved] = impl_->resolve_function(function.module, function.space, function.name, true);
    if (!resolved)
        throw Error(std::format("unresolved script function '{}'", function.name));
    return impl_->add_thread(module, *resolved, args, Value::undefined());
}

auto VM::thread_status(ThreadRef thread) const -> ThreadStatus
{
    auto const value = impl_->thread_by_id(thread.id);
    return value ? value->status : ThreadStatus::unknown;
}

auto VM::thread_result(ThreadRef thread) const -> Value
{
    auto const value = impl_->thread_by_id(thread.id);
    if (!value)
        throw Error(std::format("unknown thread {}", thread.id));
    return value->result;
}

auto VM::run_until_idle() -> std::size_t
{
    return impl_->run_scheduler(false, 0, false);
}

auto VM::run_until_complete() -> std::size_t
{
    return impl_->run_scheduler(true, 0, true);
}

auto VM::tick(double delta_seconds) -> std::size_t
{
    if (impl_->debug.paused)
        return 0;

    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0)
        throw Error("tick delta must be a finite, non-negative number");

    if (impl_->cfg.execution_mode == ExecutionMode::tick_based)
    {
        auto const period = impl_->tick_period();
        auto const steps = std::max<std::uint64_t>(1,
            static_cast<std::uint64_t>(std::ceil((delta_seconds - time_epsilon) / period)));
        delta_seconds = static_cast<double>(steps) * period;
    }

    impl_->clock += delta_seconds;
    impl_->real_clock += delta_seconds;

    for (auto& thread : impl_->threads)
    {
        if (thread->status == ThreadStatus::waiting_frame)
            thread->status = ThreadStatus::ready;
    }

    impl_->wake_timed();
    return impl_->run_ready(impl_->cfg.instruction_limit);
}

auto VM::notify(Value const& target, std::string_view event, std::span<Value const> args) -> std::size_t
{
    return impl_->dispatch(target, event, args);
}

auto VM::has_runnable_threads() const -> bool
{
    return std::any_of(impl_->threads.begin(), impl_->threads.end(), [](auto const& thread) { return thread->status == ThreadStatus::ready; });
}

auto VM::has_live_threads() const -> bool
{
    return impl_->has_live_threads();
}

auto VM::time() const -> double { return impl_->clock; }

auto VM::set_instruction_limit(std::size_t limit) -> void { impl_->cfg.instruction_limit = limit; }
auto VM::set_max_call_depth(std::size_t depth) -> void { impl_->cfg.max_call_depth = depth; }

auto VM::set_execution_mode(ExecutionMode mode) -> void
{
    if (mode == ExecutionMode::tick_based && (!(impl_->cfg.tick_rate > 0.0) || !std::isfinite(impl_->cfg.tick_rate)))
        throw Error("tick rate must be a positive finite number");

    impl_->cfg.execution_mode = mode;
}

auto VM::set_tick_rate(double ticks_per_second) -> void
{
    if (!(ticks_per_second > 0.0) || !std::isfinite(ticks_per_second))
        throw Error("tick rate must be a positive finite number");

    impl_->cfg.tick_rate = ticks_per_second;
}

auto VM::set_real_time(bool enabled) -> void { impl_->cfg.real_time = enabled; }

auto VM::instruction_count() const -> std::size_t { return impl_->executed; }

auto VM::level_object() -> ObjectPtr
{
    impl_->ensure_globals();
    return impl_->level;
}

auto VM::game_object() -> ObjectPtr
{
    impl_->ensure_globals();
    return impl_->game;
}

auto VM::world_object() -> ObjectPtr
{
    impl_->ensure_globals();
    return impl_->world;
}

auto VM::classes_object() -> ObjectPtr
{
    impl_->ensure_globals();
    return impl_->classes;
}

auto VM::anim_object() -> ObjectPtr
{
    impl_->ensure_globals();
    return impl_->anim;
}

namespace
{

struct InspectState
{
    InspectOptions options;
    std::size_t arrays{};
    std::size_t objects{};
    std::size_t values{};
    std::size_t nested_values{};
    std::size_t estimated_bytes{};
    std::unordered_set<void const*> seen;
};

auto container_pointer(Value const& value) -> void const*
{
    if (value.is_array())
        return value.as_array_ptr().get();
    if (value.is_object())
        return value.as_object_ptr().get();
    return nullptr;
}

auto inspect_value_impl(Value const& value, std::string name, std::size_t depth, InspectState& state) -> VariableInfo
{
    state.values++;
    state.estimated_bytes += name.size() + 32;

    auto info = VariableInfo{};
    info.name = std::move(name);
    info.type = std::string{ value.type_name() };

    if (value.is_array() || value.is_object())
    {
        auto const is_array = value.is_array();
        auto const keys = is_array ? value.as_array().keys() : value.as_object().keys();
        auto const* pointer = container_pointer(value);

        info.child_count = keys.size();
        info.expandable = true;
        state.estimated_bytes += keys.size() * (sizeof(Value) + 16);

        if (is_array)
        {
            state.arrays++;
            info.value = std::format("array(size={})", keys.size());
        }
        else
        {
            state.objects++;
            info.value = std::format("object({}, size={})", value.as_object().class_name(), keys.size());
        }

        if (pointer && state.seen.contains(pointer))
        {
            info.value += " <cycle>";
            info.expandable = false;
            return info;
        }

        if (depth >= state.options.max_depth)
            return info;

        if (pointer)
            state.seen.insert(pointer);

        auto const count = std::min(keys.size(), state.options.max_items);
        info.children.reserve(count);
        for (std::size_t i = 0; i < count; i++)
        {
            auto const child = is_array ? value.as_array().get(keys[i]) : value.as_object().get_index(keys[i]);
            auto child_name = keys[i].to_display_string();
            info.children.push_back(inspect_value_impl(child, std::move(child_name), depth + 1, state));
            state.nested_values++;
        }

        if (keys.size() > count)
            info.value += std::format(" ({} more)", keys.size() - count);

        if (pointer)
            state.seen.erase(pointer);
        return info;
    }

    try
    {
        info.value = value.to_display_string();
    }
    catch (...)
    {
        info.value = "<unprintable>";
    }
    state.estimated_bytes += info.value.size();
    return info;
}

auto make_stack_info(StackItem const& item, std::size_t index, InspectState& state) -> VariableInfo
{
    switch (item.kind)
    {
        case StackItem::Kind::value:
            return inspect_value_impl(item.value, std::format("stack[{}]", index), 0, state);
        case StackItem::Kind::reference:
        {
            state.values++;
            auto info = VariableInfo{};
            info.name = std::format("stack[{}]", index);
            info.type = "reference";
            info.value = "variable reference";
            return info;
        }
        case StackItem::Kind::marker:
        {
            state.values++;
            auto info = VariableInfo{};
            info.name = std::format("stack[{}]", index);
            info.type = "marker";
            info.value = "call marker";
            return info;
        }
    }

    return VariableInfo{};
}

auto make_frame_info(Frame const& frame, InspectOptions options) -> FrameInfo
{
    auto out = FrameInfo{};
    out.module = frame.module ? frame.module->name : std::string{};
    out.function = frame.function ? frame.function->name : std::string{};
    out.pc = frame.pc;
    out.max_stack = frame.max_stack;

    if (frame.function && !frame.function->instructions.empty())
    {
        auto const index = std::min(frame.pc, frame.function->instructions.size() - 1);
        out.offset = frame.function->instructions[index].offset;
        out.opcode = frame.function->instructions[index].name;
    }

    auto state = InspectState{};
    state.options = options;

    out.locals.reserve(frame.locals.size());
    for (std::size_t i = 0; i < frame.locals.size(); i++)
    {
        auto name = i < frame.local_names.size() && !frame.local_names[i].empty()
            ? frame.local_names[i]
            : std::format("local{}", i);
        auto const& value = frame.locals[frame.locals.size() - 1 - i];
        out.locals.push_back(inspect_value_impl(value, std::move(name), 0, state));
    }

    out.varargs.reserve(frame.varargs.size());
    for (std::size_t i = 0; i < frame.varargs.size(); i++)
        out.varargs.push_back(inspect_value_impl(frame.varargs[i], std::format("vararg[{}]", i), 0, state));

    out.stack.reserve(frame.stack.size());
    for (std::size_t i = 0; i < frame.stack.size(); i++)
        out.stack.push_back(make_stack_info(frame.stack[i], i, state));

    return out;
}

auto hex_preview(std::span<std::uint8_t const> bytes, std::size_t limit) -> std::string
{
    static constexpr char digits[] = "0123456789abcdef";
    auto out = std::string{};
    auto const count = std::min(bytes.size(), limit);
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; i++)
    {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0f]);
    }
    if (bytes.size() > count)
        out += "...";
    return out;
}

auto hex_preview(std::string_view text, std::size_t limit) -> std::string
{
    return hex_preview(std::span<std::uint8_t const>{ reinterpret_cast<std::uint8_t const*>(text.data()), text.size() }, limit);
}

auto collect_memory_blocks(Value const& value, std::string name, std::size_t preview_bytes,
    std::unordered_set<void const*>& seen, std::vector<MemoryBlockInfo>& out) -> void
{
    if (!value.is_array() && !value.is_object())
        return;

    auto const* pointer = container_pointer(value);
    if (!pointer || seen.contains(pointer))
        return;
    seen.insert(pointer);

    auto text = std::string{};
    try
    {
        text = value.to_string();
    }
    catch (...)
    {
        text = "<unprintable>";
    }
    auto const preview = text.size() > 4096 ? text.substr(0, 4096) : text;
    auto const base_name = name;

    out.push_back(MemoryBlockInfo{
        .kind = value.is_array() ? "array" : "object",
        .name = std::move(name),
        .size = text.size(),
        .preview_hex = hex_preview(preview, preview_bytes),
    });

    auto const keys = value.is_array() ? value.as_array().keys() : value.as_object().keys();
    for (auto const& key : keys)
    {
        auto const child = value.is_array() ? value.as_array().get(key) : value.as_object().get_index(key);
        collect_memory_blocks(child, std::format("{}[{}]", base_name, key.to_display_string()), preview_bytes, seen, out);
    }

    seen.erase(pointer);
}

} // namespace

auto VM::clear_search_paths() -> void
{
    impl_->cfg.search_paths.clear();
}

auto VM::add_virtual_file(std::string path, std::span<std::uint8_t const> data) -> void
{
    auto key = normalize_virtual_path(path);
    if (key.empty())
        throw Error("virtual file path cannot be empty");
    impl_->virtual_files.insert_or_assign(std::move(key), std::vector<std::uint8_t>{ data.begin(), data.end() });
}

auto VM::add_virtual_file(std::string path, std::string_view text) -> void
{
    add_virtual_file(std::move(path), std::span<std::uint8_t const>{ reinterpret_cast<std::uint8_t const*>(text.data()), text.size() });
}

auto VM::remove_virtual_file(std::string_view path) -> bool
{
    auto const key = impl_->find_virtual_path(path);
    if (!key)
        return false;
    return impl_->virtual_files.erase(*key) != 0;
}

auto VM::clear_virtual_files() -> void
{
    impl_->virtual_files.clear();
}

auto VM::virtual_files() const -> std::vector<std::string>
{
    auto out = std::vector<std::string>{};
    out.reserve(impl_->virtual_files.size());
    for (auto const& [path, data] : impl_->virtual_files)
    {
        (void)data;
        out.push_back(path);
    }
    return out;
}

auto VM::resolve_virtual_path(std::string_view path) const -> std::optional<std::string>
{
    return impl_->find_virtual_path(path);
}

auto VM::read_virtual_file(std::string_view path) const -> std::optional<std::vector<std::uint8_t>>
{
    auto const key = impl_->find_virtual_path(path);
    if (!key)
        return std::nullopt;
    auto const it = impl_->virtual_files.find(*key);
    if (it == impl_->virtual_files.end())
        return std::nullopt;
    return it->second;
}

auto VM::module_bytes(std::string_view name) const -> std::vector<std::uint8_t>
{
    auto const module = impl_->find_module(name);
    if (!module)
        throw Error(std::format("unknown module '{}'", name));
    return module->bytes;
}

auto VM::thread_info(ThreadRef thread, InspectOptions options) const -> ThreadInfo
{
    auto const runtime = impl_->thread_by_id(thread.id);
    if (!runtime)
        throw Error(std::format("unknown thread {}", thread.id));

    auto out = ThreadInfo{};
    out.thread = thread;
    out.status = runtime->status;
    out.wake_time = runtime->wake_time;
    out.wait_event = runtime->wait_event;
    out.wait_target = runtime->wait_object.to_display_string();
    out.result = runtime->result.to_display_string();
    out.error = runtime->error;
    out.endon_count = runtime->endons.size();

    out.frames.reserve(runtime->frames.size());
    for (auto it = runtime->frames.rbegin(); it != runtime->frames.rend(); ++it)
        out.frames.push_back(make_frame_info(*it, options));

    return out;
}

auto VM::threads(InspectOptions options) const -> std::vector<ThreadInfo>
{
    auto out = std::vector<ThreadInfo>{};
    out.reserve(impl_->threads.size());
    for (auto const& thread : impl_->threads)
        out.push_back(thread_info(ThreadRef{ thread->id }, options));
    return out;
}

auto VM::globals(InspectOptions options) const -> std::vector<VariableInfo>
{
    auto* self = const_cast<Impl*>(impl_.get());
    self->ensure_globals();

    auto out = std::vector<VariableInfo>{};
    auto state = InspectState{};
    state.options = options;
    auto add = [&](ObjectPtr const& object, std::string name) {
        if (object)
            out.push_back(inspect_value_impl(Value::object(object), std::move(name), 0, state));
    };
    add(self->level, "level");
    add(self->game, "game");
    add(self->world, "world");
    add(self->classes, "classes");
    add(self->anim, "anim");
    return out;
}

auto VM::inspect_value(Value const& value, InspectOptions options) const -> VariableInfo
{
    auto state = InspectState{};
    state.options = options;
    return inspect_value_impl(value, {}, 0, state);
}

auto VM::memory_stats() const -> MemoryStats
{
    auto* self = const_cast<Impl*>(impl_.get());
    self->ensure_globals();

    auto stats = MemoryStats{};
    stats.modules = self->modules.size();
    stats.threads = self->threads.size();

    for (auto const& module : self->modules)
    {
        stats.functions += module->functions.size();
        stats.module_bytes += module->bytes.size();
        stats.estimated_bytes += module->bytes.size() + module->disassembly.size() + module->name.size();
        for (auto const& function : module->functions)
        {
            stats.instructions += function.instructions.size();
            stats.estimated_bytes += function.name.size() + function.space.size();
            for (auto const& instruction : function.instructions)
                stats.estimated_bytes += instruction.name.size() + instruction.data.size() * sizeof(std::string);
        }
    }

    auto state = InspectState{};
    state.options = InspectOptions{ 8, 1024 };
    auto add = [&](Value const& value, std::string name) {
        (void)inspect_value_impl(value, std::move(name), 0, state);
    };

    if (self->level) add(Value::object(self->level), "level");
    if (self->game) add(Value::object(self->game), "game");
    if (self->world) add(Value::object(self->world), "world");
    if (self->classes) add(Value::object(self->classes), "classes");
    if (self->anim) add(Value::object(self->anim), "anim");

    for (auto const& thread : self->threads)
    {
        if (!Impl::terminal_status(thread->status))
            stats.live_threads++;
        stats.frames += thread->frames.size();
        stats.estimated_bytes += sizeof(Thread) + thread->error.size() + thread->wait_event.size();

        add(thread->self, std::format("thread[{}].self", thread->id));
        add(thread->result, std::format("thread[{}].result", thread->id));
        add(thread->wait_object, std::format("thread[{}].wait", thread->id));

        for (std::size_t i = 0; i < thread->frames.size(); i++)
        {
            auto const& frame = thread->frames[i];
            stats.locals += frame.locals.size();
            stats.stack_items += frame.stack.size();
            stats.estimated_bytes += sizeof(Frame) + frame.stack.capacity() * sizeof(StackItem)
                + frame.locals.capacity() * sizeof(Value) + frame.varargs.capacity() * sizeof(Value);

            add(frame.self, std::format("thread[{}].frame[{}].self", thread->id, i));
            for (std::size_t j = 0; j < frame.locals.size(); j++)
                add(frame.locals[j], std::format("thread[{}].frame[{}].local[{}]", thread->id, i, j));
            for (std::size_t j = 0; j < frame.varargs.size(); j++)
                add(frame.varargs[j], std::format("thread[{}].frame[{}].vararg[{}]", thread->id, i, j));
            for (std::size_t j = 0; j < frame.stack.size(); j++)
            {
                if (frame.stack[j].kind == StackItem::Kind::value)
                    add(frame.stack[j].value, std::format("thread[{}].frame[{}].stack[{}]", thread->id, i, j));
                else
                    state.values++;
            }
        }

        for (auto const& endon : thread->endons)
            add(endon.object, std::format("thread[{}].endon[{}]", thread->id, endon.event));
    }

    stats.arrays = state.arrays;
    stats.objects = state.objects;
    stats.values = state.values;
    stats.nested_values = state.nested_values;
    stats.estimated_bytes += state.estimated_bytes;
    return stats;
}

auto VM::memory_blocks(std::size_t preview_bytes) const -> std::vector<MemoryBlockInfo>
{
    auto out = std::vector<MemoryBlockInfo>{};

    for (auto const& [path, data] : impl_->virtual_files)
    {
        out.push_back(MemoryBlockInfo{
            .kind = "file",
            .name = path,
            .size = data.size(),
            .preview_hex = hex_preview(std::span<std::uint8_t const>{ data.data(), data.size() }, preview_bytes),
        });
    }

    for (auto const& module : impl_->modules)
    {
        out.push_back(MemoryBlockInfo{
            .kind = "module",
            .name = module->name,
            .size = module->bytes.size(),
            .preview_hex = hex_preview(std::span<std::uint8_t const>{ module->bytes.data(), module->bytes.size() }, preview_bytes),
        });
    }

    auto* self = const_cast<Impl*>(impl_.get());
    self->ensure_globals();

    auto seen = std::unordered_set<void const*>{};
    auto add = [&](Value const& value, std::string name) {
        collect_memory_blocks(value, std::move(name), preview_bytes, seen, out);
    };
    if (self->level) add(Value::object(self->level), "level");
    if (self->game) add(Value::object(self->game), "game");
    if (self->world) add(Value::object(self->world), "world");
    if (self->classes) add(Value::object(self->classes), "classes");
    if (self->anim) add(Value::object(self->anim), "anim");

    for (auto const& thread : self->threads)
    {
        add(thread->self, std::format("thread[{}].self", thread->id));
        add(thread->result, std::format("thread[{}].result", thread->id));
        for (std::size_t i = 0; i < thread->frames.size(); i++)
        {
            auto const& frame = thread->frames[i];
            add(frame.self, std::format("thread[{}].frame[{}].self", thread->id, i));
            for (std::size_t j = 0; j < frame.locals.size(); j++)
                add(frame.locals[j], std::format("thread[{}].frame[{}].local[{}]", thread->id, i, j));
            for (std::size_t j = 0; j < frame.varargs.size(); j++)
                add(frame.varargs[j], std::format("thread[{}].frame[{}].vararg[{}]", thread->id, i, j));
            for (std::size_t j = 0; j < frame.stack.size(); j++)
            {
                if (frame.stack[j].kind == StackItem::Kind::value)
                    add(frame.stack[j].value, std::format("thread[{}].frame[{}].stack[{}]", thread->id, i, j));
            }
        }
    }

    return out;
}

auto VM::set_breakpoint(std::string_view module, std::string_view function, std::size_t pc) -> bool
{
    auto [resolved_module, resolved_function] = impl_->resolve_function(module, {}, function, true);
    if (!resolved_module || !resolved_function || pc >= resolved_function->instructions.size())
        return false;

    auto breakpoint = BreakpointInfo{ resolved_module->name, resolved_function->name, pc };
    if (std::find(impl_->debug.breakpoints.begin(), impl_->debug.breakpoints.end(), breakpoint) == impl_->debug.breakpoints.end())
        impl_->debug.breakpoints.push_back(std::move(breakpoint));

    impl_->debug.enabled = true;
    return true;
}

auto VM::clear_breakpoints() -> void
{
    impl_->debug.breakpoints.clear();
}

auto VM::breakpoints() const -> std::vector<BreakpointInfo>
{
    return impl_->debug.breakpoints;
}

auto VM::debug_state() const -> DebugState
{
    auto out = DebugState{};
    out.enabled = impl_->debug.enabled;
    out.paused = impl_->debug.paused;
    out.breakpoints = impl_->debug.breakpoints;

    if (out.paused)
    {
        auto const thread = impl_->thread_by_id(impl_->debug.paused_thread);
        if (thread && thread->status != ThreadStatus::paused)
        {
            out.paused = false;
        }
        else if (thread)
        {
            out.thread = ThreadRef{ thread->id };
            if (!thread->frames.empty())
            {
                auto const& frame = thread->frames.back();
                out.module = frame.module ? frame.module->name : std::string{};
                out.function = frame.function ? frame.function->name : std::string{};
                out.pc = frame.pc;
                if (frame.function && !frame.function->instructions.empty())
                {
                    auto const index = std::min(frame.pc, frame.function->instructions.size() - 1);
                    out.offset = frame.function->instructions[index].offset;
                    out.opcode = frame.function->instructions[index].name;
                }
            }
        }
    }

    return out;
}

auto VM::set_debug_enabled(bool enabled) -> void
{
    impl_->debug.enabled = enabled;
    if (!enabled)
    {
        impl_->debug.pause_requested = false;
        impl_->debug.stepping = false;
        impl_->debug.paused = false;
        impl_->debug.paused_thread = 0;
        for (auto& thread : impl_->threads)
        {
            if (thread->status == ThreadStatus::paused)
                thread->status = ThreadStatus::ready;
        }
    }
}

auto VM::step() -> std::size_t
{
    if (!impl_->debug.enabled || !impl_->debug.paused)
        return 0;

    auto const thread = impl_->thread_by_id(impl_->debug.paused_thread);
    if (!thread || thread->status != ThreadStatus::paused)
        return 0;

    auto const start = impl_->executed;
    impl_->debug.paused = false;
    impl_->debug.paused_thread = 0;
    impl_->debug.stepping = true;
    impl_->debug.step_thread = thread->id;
    thread->status = ThreadStatus::ready;

    auto budget = impl_->cfg.instruction_limit == 0 ? std::numeric_limits<std::size_t>::max() : impl_->cfg.instruction_limit;
    try
    {
        impl_->run_thread(*thread, budget);
    }
    catch (...)
    {
        impl_->debug.stepping = false;
        throw;
    }

    if (impl_->debug.stepping)
        impl_->debug.stepping = false;
    if (thread->status == ThreadStatus::running)
        impl_->pause_thread(*thread);

    return impl_->executed - start;
}

auto VM::resume() -> std::size_t
{
    if (!impl_->debug.enabled)
        impl_->debug.enabled = true;

    auto const start = impl_->executed;
    auto primary = std::uint64_t{};

    if (impl_->debug.paused)
    {
        auto const thread = impl_->thread_by_id(impl_->debug.paused_thread);
        if (thread && thread->status == ThreadStatus::paused)
        {
            primary = thread->id;
            thread->status = ThreadStatus::ready;
        }
        impl_->debug.paused = false;
        impl_->debug.paused_thread = 0;
    }

    impl_->debug.stepping = false;
    impl_->debug.pause_requested = false;
    impl_->run_scheduler(true, primary, false);
    return impl_->executed - start;
}

auto VM::pause() -> void
{
    impl_->debug.enabled = true;
    if (!impl_->debug.paused)
        impl_->debug.pause_requested = true;
}

auto VM::get_global(std::string_view name) const -> Value
{
    if (!impl_->level)
        return Value::undefined();
    return impl_->level->get_field(name);
}

auto VM::set_global(std::string_view name, Value value) -> void
{
    impl_->ensure_globals();
    impl_->level->set_field(name, std::move(value));
}

} // namespace gsc::t6
