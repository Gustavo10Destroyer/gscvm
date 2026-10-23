# Extensibility and host integration

The VM owns bytecode semantics and cooperative scheduling. Everything that
belongs to a game engine is supplied through a `Host`, the native registry, or
custom `Object` subclasses.

## Host services

Implement `gsc::t6::Host` and pass a shared instance in `VMConfig::host`:

```cpp
class MyHost final : public gsc::t6::Host
{
public:
    auto log(gsc::t6::LogLevel level, std::string_view message) -> void override
    {
        // Route VM diagnostics to the engine console.
    }

    auto dvar(std::string_view name) const -> gsc::t6::Value override
    {
        // Return undefined when the dvar is absent.
        return gsc::t6::Value::undefined();
    }

    auto load_script(std::string_view name) const
        -> std::optional<std::vector<std::uint8_t>> override
    {
        // Optional bytecode loader used after the VM's search paths.
        return std::nullopt;
    }

    auto level_object() const -> gsc::t6::ObjectPtr override
    {
        return level_;
    }

    auto create_object(std::string_view class_name) -> gsc::t6::ObjectPtr override
    {
        return std::make_shared<gsc::t6::ScriptObject>(std::string{ class_name });
    }

private:
    gsc::t6::ObjectPtr level_ = std::make_shared<gsc::t6::ScriptObject>("level");
};
```

`DefaultHost` is a small headless implementation: it logs to stderr, keeps an
in-memory dvar table and loads scripts from the configured search paths. Use it
for tools and tests, or subclass it to add engine services.

### Host methods

| Method | Purpose |
| --- | --- |
| `log()` | Receives VM log messages and levels. |
| `dvar()` | Returns a typed dvar value, or undefined when absent. |
| `dvar_int()`, `dvar_float()`, `dvar_vector()` | Typed dvar access with host-defined defaults. |
| `load_script()` | Optional bytecode loader consulted by dependency resolution. |
| `level_object()`, `game_object()`, `world_object()`, `classes_object()`, `anim_object()` | Engine singletons. Null results make the VM use a private `ScriptObject`. |
| `create_object()` | Creates objects for object-producing opcodes and natives. |

`DefaultHost::set_dvar()` is available when using the default host directly.

## Native functions

Native functions are registered per VM:

```cpp
vm.natives().register_function("", "host_time", [](NativeContext& ctx) {
    ctx.set_return_value(Value::floating(static_cast<float>(ctx.vm().time())));
});

vm.natives().register_function("math", "clamp", [](NativeContext& ctx) {
    auto const value = ctx.get_argument<float>(0);
    auto const low = ctx.get_argument<float>(1);
    auto const high = ctx.get_argument<float>(2);
    ctx.set_return_value(Value::floating(std::clamp(value, low, high)));
});
```

Registration and lookup use the canonical `space::name` form, with an empty
space represented as the bare name. Registration replaces an existing entry
with the same key. `unregister_function()`, `contains()` and `names()` manage the
registry. See [Native C++ bindings](native-bindings.md) for the full argument and
error API.

Script imports resolve in this order: the current module, the import namespace
as a module, the entry module, every loaded module, a module loaded from the host
or search paths, and finally the native registry. A native therefore acts as the
fallback for engine builtins.

## Custom objects

Subclass `Object` to expose engine entities without copying them into the VM:

```cpp
class Entity final : public gsc::t6::Object
{
public:
    explicit Entity(std::string name) : name_(std::move(name)) {}

    auto class_name() const -> std::string_view override { return "entity"; }

    auto to_string() const -> std::string override
    {
        return std::format("<entity {}>", name_);
    }

    auto get_field(std::string_view field) const -> gsc::t6::Value override
    {
        if (field == "name")
            return gsc::t6::Value::string(name_);
        return gsc::t6::Value::undefined();
    }

private:
    std::string name_;
};
```

`Object` also supports indexed access, field clearing, key iteration, size and
identity (`same_as`). `Value` and `ObjectPtr` are shared handles, so an object
returned to script remains alive as long as a script or native retains it.

`to_string()` is used by string concatenation and console formatting. Override it
to give native objects stable, readable output. `debug_name()` is available for
diagnostics and inspection.

## Script resolution

The VM's search order for a requested script is:

1. in-memory virtual files,
2. `Host::load_script()`,
3. the VM's filesystem search paths.

Source includes use the same virtual file and search path machinery. Add paths
with `add_search_path()`; remove all of them with `clear_search_paths()`. Virtual
files take priority, which lets an editor keep unsaved buffers in front of disk
files.

## Error policy

Host and native code should throw `gsc::Error` (or a subclass) for expected
failures. `NativeContext::error()` raises a `RuntimeError` with the current
module, function, instruction and thread context. Exceptions from a native
propagate as execution failures; the scheduler does not swallow them.

## Current scope

gscvm currently implements the T6 dialect only. The upstream compiler,
disassembler and opcode tables are reused, but the interpreter, scheduler and
public API are T6-specific. Extending to another dialect would require a new
module decoder/IR mapping and opcode semantics, not just a new Host.
