# Native C++ bindings

Natives bridge GSC imports to C++ code. They are registered on a specific `VM`,
so separate VMs never share native state.

## Registering a native

```cpp
#include <algorithm>
#include <format>

#include "gsc/gscvm.hpp"

using namespace gsc::t6;

vm.natives().register_function("", "echo", [](NativeContext& ctx) {
    ctx.set_return_value(ctx.argument(0));
});

vm.natives().register_function("math", "clamp", [](NativeContext& ctx) {
    auto const value = ctx.get_argument<float>(0);
    auto const low = ctx.get_argument<float>(1);
    auto const high = ctx.get_argument<float>(2);
    ctx.set_return_value(Value::floating(std::clamp(value, low, high)));
});
```

GSC calls the first as `echo(...)` and the second as `math::clamp(...)`.
Registering the same canonical name again replaces the previous function.

Including a native space makes its natives available unqualified:

```gsc
#include browser;

main()
{
    return storage("name");
}
```

If several included spaces define the same name, the first include wins.
`space::name` always selects that exact native; it does not fall back to a bare
native with the same name.

## Arguments

`NativeContext` exposes the call as copied `Value` arguments:

```cpp
ctx.argument_count();
ctx.argument(index);
ctx.self();
ctx.return_value();
ctx.set_return_value(value);
ctx.return_undefined();
```

Use the typed helper when the script is expected to pass a compatible value:

```cpp
auto name = ctx.get_argument<std::string>(0);
auto count = ctx.get_argument<std::int32_t>(1);
auto enabled = ctx.get_argument<bool>(2);
auto origin = ctx.get_argument<Vector3>(3);
auto value = ctx.get_argument<Value>(4);
```

Supported types are `Value`, `std::string`, `std::string_view`, `bool`, integral
types, floating-point types and `Vector3`. Invalid conversions throw an error
with the argument index and actual type. `try_get_argument<T>()` returns
`std::nullopt` instead of throwing.

Argument strings are owned copies. Arrays, objects and threads are shared
handles; retaining a `Value` keeps the underlying data alive. `self()` is the
method receiver for method calls.

## Return values

```cpp
ctx.set_return_value(Value::integer(42));
ctx.set_return_value(Value::string("ready"));
ctx.set_return_value(Value::array());
ctx.return_undefined();
```

Returning without setting a value leaves the native's result undefined. The
`Value` factory functions cover undefined, boolean, integer, float, string,
istring, hash, vector, array, object, function, thread, animtree and animation
values.

## Arrays and objects

```cpp
vm.natives().register_function("", "make_pair", [](NativeContext& ctx) {
    auto result = std::make_shared<Array>();
    result->set(Value::integer(0), ctx.argument(0));
    result->set(Value::integer(1), ctx.argument(1));
    ctx.set_return_value(Value::array(result));
});
```

`Array` preserves insertion order for iteration and supports integer or string
keys. `Object` and `ScriptObject` provide field/index access. See
[Extensibility](extensibility.md) for custom object subclasses.

## Errors

Raise a script-visible runtime error with `NativeContext::error()`:

```cpp
vm.natives().register_function("", "require_positive", [](NativeContext& ctx) {
    if (ctx.get_argument<std::int32_t>(0) <= 0)
        ctx.error("value must be positive");
});
```

The error includes the current module, function, instruction offset and thread
id. You can also throw `gsc::Error`; execution reports it through the normal
exception path.

## Context information

```cpp
ctx.vm();            // VM&
ctx.function_space();
ctx.function_name();
ctx.thread_id();
```

`ctx.vm()` gives access to VM services such as time, globals, search paths and
scheduling. Do not retain a `NativeContext` reference beyond the call.

## Registry management

```cpp
vm.natives().contains("echo");
vm.natives().contains("math", "clamp");
vm.natives().names();
vm.natives().unregister_function("echo");
vm.natives().unregister_function("math", "clamp");
```

The registry is per VM and is not synchronized. Register or modify it from the
same thread that drives the VM.

## CLI builtins

The command-line runner registers `print`, `println`, `iprintln`,
`iprintlnbold` and `gettime`. The WASM target registers the print family and
`gettime` as well, routing output through `gscvm_take_output()`. WebAssembly
embedders can add JavaScript natives with the SDK's `registerNative()` or the
lower-level `gscvm_register_js_native()`. Other natives are the embedder's
responsibility, which keeps the VM headless and free of engine assumptions.
