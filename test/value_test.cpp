// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include <catch_amalgamated.hpp>

#include <array>
#include <memory>

#include "gsc/gscvm.hpp"

namespace gsc::test
{

using namespace gsc::t6;

TEST_CASE("values compare and convert", "[gscvm]")
{
    REQUIRE(Value::integer(3).equals(Value::floating(3.0f)));
    REQUIRE_FALSE(Value::integer(3).equals(Value::floating(3.0f), true));
    REQUIRE(Value::string("3").equals(Value::istring("3")));
    REQUIRE(Value::integer(3).to_floating() == 3.0f);
    REQUIRE(Value::boolean(true).to_integer() == 1);
    REQUIRE(Value::undefined().is_undefined());
}

TEST_CASE("arrays preserve insertion order and support erasure", "[gscvm]")
{
    auto array = Array{};
    array.set(Value::string("b"), Value::integer(2));
    array.set(Value::integer(1), Value::integer(1));
    array.set(Value::string("c"), Value::integer(3));

    REQUIRE(array.size() == 3);
    REQUIRE(array.first_key().as_string() == "b");
    REQUIRE(array.next_key(Value::string("b")).as_integer() == 1);
    REQUIRE(array.next_key(Value::integer(1)).as_string() == "c");
    REQUIRE(array.next_key(Value::string("c")).is_undefined());

    REQUIRE(array.erase(Value::integer(1)));
    REQUIRE(array.size() == 2);
    REQUIRE(array.get(Value::integer(1)).is_undefined());
    REQUIRE(array.next_key(Value::string("b")).as_string() == "c");
}

TEST_CASE("script objects expose fields and index keys", "[gscvm]")
{
    auto object = std::make_shared<ScriptObject>("entity");
    object->set_field("health", Value::integer(50));
    object->set_index(Value::string("name"), Value::string("actor"));

    REQUIRE(object->class_name() == "entity");
    REQUIRE(object->get_field("health").as_integer() == 50);
    REQUIRE(object->get_index(Value::string("name")).as_string() == "actor");
    REQUIRE(object->size() == 2);
    REQUIRE(object->clear_field("health"));
    REQUIRE(object->get_field("health").is_undefined());
}

TEST_CASE("native objects provide custom serialization", "[gscvm]")
{
    struct NativeObject final : Object
    {
        auto class_name() const -> std::string_view override { return "native"; }
        auto to_string() const -> std::string override { return "native:thing"; }
    };

    auto const value = Value::object(std::make_shared<NativeObject>());

    REQUIRE(value.to_string() == "native:thing");
    REQUIRE(value.to_display_string() == "native:thing");
    REQUIRE(Value::string("value=").to_string() + value.to_string() == "value=native:thing");
}

TEST_CASE("structs and levels expose detailed debug serialization", "[gscvm]")
{
    auto const nested = std::make_shared<ScriptObject>("struct");
    nested->set_field("value", Value::integer(9));

    auto const headers = std::make_shared<Array>();
    headers->set(Value::string("user-agent"), Value::string("GSCVM/beta"));

    auto const object = std::make_shared<ScriptObject>("struct");
    object->set_field("method", Value::string("GET"));
    object->set_field("headers", Value::array(headers));
    object->set_field("nested", Value::object(nested));
    object->set_field("self", Value::object(object));

    auto const text = object->to_string();

    REQUIRE(text.starts_with("struct {\n"));
    REQUIRE(text.find("  method: string = \"GET\"") != std::string::npos);
    REQUIRE(text.find("  headers: array(size=1) [\n") != std::string::npos);
    REQUIRE(text.find("    \"user-agent\": string = \"GSCVM/beta\"") != std::string::npos);
    REQUIRE(text.find("  nested: struct {\n    value: int = 9\n  }") != std::string::npos);
    REQUIRE(text.find("  self: struct {...}") != std::string::npos);
}

TEST_CASE("console formatting expands placeholders and colours", "[gscvm]")
{
    auto const arguments = std::array{ Value::vector(1, 2, 3), Value::string("z") };

    REQUIRE(format_message("v=&&1 name=&&2", arguments) == "v=(1, 2, 3) name=z");
    REQUIRE(format_message("&&3", arguments) == "&&3");
    REQUIRE(format_message("^1&&1^7", arguments) == "\x1b[31m(1, 2, 3)\x1b[0m");
    REQUIRE(apply_color_codes("^1red^7 ok") == "\x1b[31mred\x1b[0m ok");
    REQUIRE(apply_color_codes("^2green") == "\x1b[32mgreen\x1b[0m");
    REQUIRE(apply_color_codes("plain") == "plain");
}

TEST_CASE("default host stores dvars", "[gscvm]")
{
    auto host = DefaultHost{};
    host.set_dvar("sv_test", Value::integer(7));
    host.set_dvar("gravity", Value::floating(800.0f));

    REQUIRE(host.dvar_int("sv_test") == 7);
    REQUIRE(host.dvar_float("gravity") == 800.0f);
    REQUIRE(host.dvar("missing").is_undefined());
}

} // namespace gsc::test
