// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "common.hpp"

namespace gsc::test
{

using namespace gsc::t6;

TEST_CASE("t6 loader exposes module metadata and disassembly", "[gscvm]")
{
    auto vm = make_vm();
    auto const info = vm.load_module_file(fixture("basic.gsc"), "basic");

    REQUIRE(info.name == "basic.gsc");
    REQUIRE(vm.has_module("basic"));
    REQUIRE(vm.has_module("basic.gsc"));
    REQUIRE(vm.disassembly("basic").find("sub:main") != std::string::npos);
    REQUIRE(info.functions.size() == 8);
}

TEST_CASE("t6 dependencies load from search paths", "[gscvm]")
{
    auto vm = make_vm();
    vm.add_search_path(fixture(""));
    vm.load_module_file(fixture("dep.gsc"), "dep");

    vm.load_dependencies("dep");
    REQUIRE(vm.has_module("dep_helper"));
    REQUIRE(vm.call("dep", "main").as_integer() == 77);
}

TEST_CASE("t6 namespaced natives do not load a script module", "[gscvm]")
{
    auto vm = make_vm();
    vm.natives().register_function("browser", "storage", [](NativeContext& context) {
        context.set_return_value(Value::string(context.get_argument<std::string>(0)));
    });

    auto const source = std::string{ "main() { return browser::storage(\"name\"); }" };
    vm.load_module("native_space", std::vector<std::uint8_t>{ source.begin(), source.end() });

    REQUIRE_NOTHROW(vm.load_dependencies("native_space"));
    REQUIRE_FALSE(vm.has_module("browser"));
    REQUIRE(vm.call("native_space", "main").as_string() == "name");
}

TEST_CASE("t6 compiles source scripts on load", "[gscvm]")
{
    auto vm = make_vm();
    auto const info = vm.load_module_file(source_fixture("basic.gsc"), "basic");

    REQUIRE(info.name == "basic.gsc");
    REQUIRE(info.functions.size() == 8);
    REQUIRE(vm.call("main").as_integer() == 7);
}

TEST_CASE("t6 compiles source dependencies on demand", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("dep.gsc"), "dep");

    vm.load_dependencies("dep");
    REQUIRE(vm.has_module("dep_helper"));
    REQUIRE(vm.call("dep", "main").as_integer() == 77);
}

TEST_CASE("t6 serializes native values and spawnstruct objects", "[gscvm]")
{
    auto vm = make_vm();
    REQUIRE(vm.natives().contains("spawnstruct"));
    vm.load_module_file(source_fixture("objects.gsc"), "objects");

    REQUIRE(vm.call("vector_string").as_string() == "v=(1, 2, 3)");
    REQUIRE(vm.call("struct_test").as_string() == "GET|GSCVM/beta|9|text/plain");

    auto const level = vm.call("level_debug").to_string();
    REQUIRE(level.find("level {") != std::string::npos);
    REQUIRE(level.find("test_value: int = 5") != std::string::npos);
}

TEST_CASE("t6 type predicates and array field restrictions", "[gscvm]")
{
    auto vm = make_vm();

    for (auto const* name : { "isarray", "isstring", "isint", "isfloat" })
        REQUIRE(vm.natives().contains(name));

    vm.load_module_file(source_fixture("predicates.gsc"), "predicates");

    REQUIRE(vm.call("array_value").as_integer() == 1);
    REQUIRE(vm.call("array_nonarray").as_integer() == 0);
    REQUIRE(vm.call("string_value").as_integer() == 1);
    REQUIRE(vm.call("string_nonstring").as_integer() == 0);
    REQUIRE(vm.call("int_value").as_integer() == 1);
    REQUIRE(vm.call("int_nonint").as_integer() == 0);
    REQUIRE(vm.call("float_value").as_integer() == 1);
    REQUIRE(vm.call("float_nonfloat").as_integer() == 0);
    REQUIRE(vm.call("size_property").as_integer() == 2);

    REQUIRE_THROWS_AS(vm.call("field_read"), RuntimeError);
    REQUIRE_THROWS_AS(vm.call("field_clear"), RuntimeError);
}

TEST_CASE("t6 calls, recursion and frame locals", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(fixture("basic.gsc"), "basic");

    REQUIRE(vm.call("main").as_integer() == 7);
    REQUIRE(vm.call("add", std::array{ Value::integer(3), Value::integer(4) }).as_integer() == 7);
    REQUIRE(vm.call("fact", std::array{ Value::integer(6) }).as_integer() == 720);
    REQUIRE(vm.call("loopsum", std::array{ Value::integer(10) }).as_integer() == 45);
    REQUIRE(vm.call("switchtest", std::array{ Value::integer(1) }).as_string() == "one");
    REQUIRE(vm.call("switchtest", std::array{ Value::integer(2) }).as_string() == "two");
    REQUIRE(vm.call("switchtest", std::array{ Value::integer(9) }).as_string() == "other");
}

TEST_CASE("t6 natives and method self", "[gscvm]")
{
    auto vm = make_vm();

    vm.natives().register_function("", "test_echo", [](NativeContext& context) {
        context.set_return_value(context.argument(0));
    });
    vm.natives().register_function("", "test_self", [](NativeContext& context) {
        context.set_return_value(context.self());
    });

    vm.load_module_file(fixture("basic.gsc"), "basic");

    REQUIRE(vm.call("nativecall", std::array{ Value::integer(123) }).as_integer() == 123);

    auto const result = vm.call("methodnative");
    REQUIRE(result.is_object());
    REQUIRE(result.as_object_ptr() == vm.level_object());
}

TEST_CASE("t6 array native builds arrays from varargs", "[gscvm]")
{
    auto vm = make_vm();
    REQUIRE(vm.natives().contains("array"));
    vm.load_module_file(fixture("features.gsc"), "features");

    auto const result = vm.call("array_native");
    REQUIRE(result.is_array());
    auto const& array = result.as_array();
    REQUIRE(array.size() == 3);
    REQUIRE(array.get(Value::integer(0)).as_string() == "item 1");
    REQUIRE(array.get(Value::integer(1)).as_string() == "item 2");
    REQUIRE(array.get(Value::integer(2)).as_integer() == 3);

    auto const empty = vm.call("array_empty");
    REQUIRE(empty.is_array());
    REQUIRE(empty.as_array().empty());
}

TEST_CASE("t6 arrays, foreach, vectors and methods", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(fixture("features.gsc"), "features");

    REQUIRE(vm.call("arraytest").as_integer() == 15);
    REQUIRE(vm.call("stringtest").as_string() == "ab3");
    REQUIRE(vm.call("vectest").as_floating() == Catch::Approx(12.0f));
    REQUIRE(vm.call("methodtest").as_integer() == 42);
}

TEST_CASE("t6 control flow, operators and builtins", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(fixture("control.gsc"), "control");

    REQUIRE(vm.call("strswitch", std::array{ Value::string("a") }).as_integer() == 1);
    REQUIRE(vm.call("strswitch", std::array{ Value::string("z") }).as_integer() == 3);
    REQUIRE(vm.call("nested").as_integer() == 7);
    REQUIRE(vm.call("whiletest", std::array{ Value::integer(5) }).as_integer() == 10);
    REQUIRE(vm.call("dowhiletest", std::array{ Value::integer(3) }).as_integer() == 3);
    REQUIRE(vm.call("bitops").as_integer() == 6);
    REQUIRE(vm.call("boolops").as_integer() == 1);
    REQUIRE(vm.call("ternary", std::array{ Value::integer(1) }).as_integer() == 10);
    REQUIRE(vm.call("ternary", std::array{ Value::integer(-1) }).as_integer() == 20);
    REQUIRE(vm.call("sizetest").as_integer() == 2);
    REQUIRE(vm.call("undeftest").as_integer() == 1);
    REQUIRE(vm.call("cleartest").as_integer() == 1);
    REQUIRE(vm.call("mathtest").as_floating() == Catch::Approx(11.5f));
    REQUIRE(vm.call("angles").as_floating() == Catch::Approx(1.0f));
}

TEST_CASE("t6 scheduler handles timed waits and events", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(fixture("wait.gsc"), "wait");

    SECTION("sleeper wakes on tick")
    {
        auto const thread = vm.start("wait", "sleeper");
        REQUIRE(vm.run_until_idle() > 0);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::waiting_time);

        vm.tick(0.6);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
        REQUIRE(vm.thread_result(thread).as_integer() == 7);
    }

    SECTION("run_until_complete advances timed waits")
    {
        auto const thread = vm.start("wait", "sleeper");
        REQUIRE(vm.run_until_complete() > 0);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
        REQUIRE(vm.thread_result(thread).as_integer() == 7);
        REQUIRE_FALSE(vm.has_live_threads());
    }

    SECTION("waittill receives arguments")
    {
        auto const thread = vm.start("wait", "waiter");
        REQUIRE(vm.run_until_idle() > 0);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::waiting_event);

        auto const args = std::array{ Value::integer(3), Value::integer(4) };
        REQUIRE(vm.notify(Value::object(vm.level_object()), "ping", args) == 1);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::ready);

        vm.run_until_idle();
        REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
        REQUIRE(vm.thread_result(thread).as_integer() == 7);
    }

    SECTION("waittillmatch uses the argument count")
    {
        auto const thread = vm.start("wait", "matcher");
        REQUIRE(vm.run_until_idle() > 0);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::waiting_event);

        auto const args = std::array{ Value::integer(3) };
        REQUIRE(vm.notify(Value::object(vm.level_object()), "ping", args) == 1);
        vm.run_until_idle();
        REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
        REQUIRE(vm.thread_result(thread).as_integer() == 1);
    }

    SECTION("endon aborts a thread")
    {
        auto const thread = vm.start("wait", "ender");
        REQUIRE(vm.run_until_idle() > 0);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::waiting_time);

        REQUIRE(vm.notify(Value::object(vm.level_object()), "stop") == 1);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::aborted);
    }

    SECTION("deadlocked event waiters are reported")
    {
        auto const thread = vm.start("wait", "starter");
        REQUIRE(vm.run_until_idle() > 0);
        REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
        REQUIRE(vm.has_live_threads());

        try
        {
            (void)vm.run_until_complete();
            FAIL("expected a deadlock error");
        }
        catch (RuntimeError const& e)
        {
            REQUIRE(e.function() == "waiter");
            REQUIRE(e.thread_id() != 0);
            REQUIRE(std::string(e.what()).find("deadlock") != std::string::npos);
            REQUIRE(std::string(e.what()).find("ping") != std::string::npos);
        }
    }
}

TEST_CASE("t6 spawned threads and in-script events", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    REQUIRE(vm.call("thread_main").as_integer() == 42);
    REQUIRE(vm.get_global("waiter_result").as_integer() == 34);
    REQUIRE_FALSE(vm.has_live_threads());

    auto const waiter = vm.get_global("waiter_thread");
    auto const notifier = vm.get_global("notifier_thread");
    auto const ender = vm.get_global("ender_thread");
    REQUIRE(waiter.is_thread());
    REQUIRE(notifier.is_thread());
    REQUIRE(ender.is_thread());

    REQUIRE(vm.thread_status(waiter.as_thread()) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(waiter.as_thread()).as_integer() == 7);
    REQUIRE(vm.thread_status(notifier.as_thread()) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(notifier.as_thread()).as_integer() == 1);
    REQUIRE(vm.thread_status(ender.as_thread()) == ThreadStatus::aborted);

    auto const combined = vm.start("threads", "combined");
    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.thread_status(combined) == ThreadStatus::waiting_event);
    REQUIRE(vm.notify(Value::object(vm.level_object()), "ping") == 1);
    REQUIRE(vm.thread_status(combined) == ThreadStatus::aborted);
}

TEST_CASE("t6 same-frame instances register endon before running", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    REQUIRE(vm.start("threads", "spawn_once").id != 0);
    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.get_global("once_starts").as_integer() == 1);
    REQUIRE(vm.get_global("once_steps").as_integer() == 1);
    REQUIRE(vm.has_live_threads());

    REQUIRE(vm.run_until_complete() > 0);
    REQUIRE(vm.get_global("once_starts").as_integer() == 1);
    REQUIRE(vm.get_global("once_steps").as_integer() == 3);
    REQUIRE_FALSE(vm.has_live_threads());
}

TEST_CASE("t6 endon terminates the whole synchronous call stack", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    auto const thread = vm.start("threads", "sync_wrapper");
    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.thread_status(thread) == ThreadStatus::waiting_time);
    REQUIRE(vm.level_object()->get_field("sync_steps").as_integer() == 1);

    REQUIRE(vm.notify(Value::object(vm.level_object()), "sync:stop") == 1);
    REQUIRE(vm.thread_status(thread) == ThreadStatus::aborted);

    REQUIRE(vm.run_until_complete() == 0);
    REQUIRE(vm.level_object()->get_field("sync_steps").as_integer() == 1);
    REQUIRE(vm.level_object()->get_field("sync_wrapper_after").as_integer() == 0);

    auto const self = vm.start("threads", "sync_self_wrapper");
    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.thread_status(self) == ThreadStatus::aborted);
    REQUIRE(vm.level_object()->get_field("sync_self_after").as_integer() == 0);
    REQUIRE(vm.level_object()->get_field("sync_self_notified").as_integer() == 0);
}

TEST_CASE("t6 wait suspends for a duration in seconds", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    auto const sleeper = vm.start("threads", "wait_fraction");
    auto const quick = vm.start("threads", "quick");

    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.thread_status(quick) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(quick).as_integer() == 9);
    REQUIRE(vm.thread_status(sleeper) == ThreadStatus::waiting_time);
    REQUIRE(vm.time() == Catch::Approx(0.0));

    vm.tick(0.049);
    REQUIRE(vm.thread_status(sleeper) == ThreadStatus::waiting_time);

    vm.tick(0.001);
    REQUIRE(vm.thread_status(sleeper) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(sleeper).as_integer() == 5);
    REQUIRE(vm.time() == Catch::Approx(0.05).margin(1e-6));

    auto const tenth = vm.start("threads", "wait_tenth");
    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.thread_status(tenth) == ThreadStatus::waiting_time);
    vm.tick(0.1);
    REQUIRE(vm.thread_status(tenth) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(tenth).as_integer() == 1);

    auto const sequence = vm.start("threads", "wait_sequence");
    REQUIRE(vm.run_until_complete() > 0);
    REQUIRE(vm.thread_status(sequence) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(sequence).as_integer() == 7);
    REQUIRE(vm.time() == Catch::Approx(3.2).margin(1e-6));
}

TEST_CASE("t6 tick-based scheduling quantizes waits", "[gscvm]")
{
    auto config = VMConfig{};
    config.execution_mode = ExecutionMode::tick_based;
    config.tick_rate = 20.0;
    auto vm = make_vm(config);
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    auto const short_wait = vm.start("threads", "wait_short");
    REQUIRE(vm.run_until_complete() > 0);
    REQUIRE(vm.thread_status(short_wait) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(short_wait).as_integer() == 3);
    REQUIRE(vm.time() == Catch::Approx(0.05).margin(1e-6));

    auto const frame_wait = vm.start("threads", "wait_frame");
    REQUIRE(vm.run_until_complete() > 0);
    REQUIRE(vm.thread_status(frame_wait) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(frame_wait).as_integer() == 2);
    REQUIRE(vm.time() == Catch::Approx(0.1).margin(1e-6));

    auto const tenth = vm.start("threads", "wait_tenth");
    REQUIRE(vm.run_until_complete() > 0);
    REQUIRE(vm.thread_status(tenth) == ThreadStatus::finished);
    REQUIRE(vm.time() == Catch::Approx(0.2).margin(1e-6));

    REQUIRE(vm.call("thread_main").as_integer() == 42);
    REQUIRE(vm.get_global("waiter_result").as_integer() == 34);
    REQUIRE(vm.time() == Catch::Approx(0.3).margin(1e-6));
    REQUIRE_FALSE(vm.has_live_threads());

    vm.set_execution_mode(ExecutionMode::real_time);
    auto const exact = vm.start("threads", "wait_fraction");
    REQUIRE(vm.run_until_complete() > 0);
    REQUIRE(vm.thread_status(exact) == ThreadStatus::finished);
    REQUIRE(vm.time() == Catch::Approx(0.35).margin(1e-6));
}

TEST_CASE("t6 tick-based tick() advances whole ticks", "[gscvm]")
{
    auto config = VMConfig{};
    config.execution_mode = ExecutionMode::tick_based;
    config.tick_rate = 20.0;
    auto vm = make_vm(config);
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    auto const thread = vm.start("threads", "wait_short");
    REQUIRE(vm.run_until_idle() > 0);
    REQUIRE(vm.thread_status(thread) == ThreadStatus::waiting_time);

    REQUIRE(vm.tick(0.01) > 0);
    REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(thread).as_integer() == 3);
    REQUIRE(vm.time() == Catch::Approx(0.05).margin(1e-6));
}

TEST_CASE("t6 tick-based scheduling honors the tick rate and pacing", "[gscvm]")
{
    auto config = VMConfig{};
    config.execution_mode = ExecutionMode::tick_based;
    config.tick_rate = 60.0;
    config.real_time = true;
    config.host = std::make_shared<DefaultHost>();
    auto vm = VM{ config };
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    auto const start = std::chrono::steady_clock::now();
    REQUIRE(vm.call("wait_short").as_integer() == 3);
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    REQUIRE(elapsed >= 0.01);
    REQUIRE(vm.time() == Catch::Approx(1.0 / 60.0).margin(1e-6));

    REQUIRE_THROWS_AS(vm.set_tick_rate(0.0), Error);
    REQUIRE_THROWS_AS(vm.set_tick_rate(-20.0), Error);
}

TEST_CASE("t6 real-time waits suspend the host", "[gscvm]")
{
    auto config = VMConfig{};
    config.real_time = true;
    config.host = std::make_shared<DefaultHost>();
    auto vm = VM{ config };
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    auto const start = std::chrono::steady_clock::now();
    REQUIRE(vm.call("wait_fraction").as_integer() == 5);
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    REQUIRE(elapsed >= 0.045);
    REQUIRE(vm.time() == Catch::Approx(0.05).margin(1e-6));
}

TEST_CASE("t6 runaway execution is bounded", "[gscvm]")
{
    REQUIRE(VMConfig{}.max_call_depth == 1024);

    auto config = VMConfig{};
    config.max_call_depth = 16;
    config.instruction_limit = 200'000;
    auto vm = make_vm(config);
    vm.load_module_file(source_fixture("threads.gsc"), "threads");

    try
    {
        (void)vm.call("recursion", std::array{ Value::integer(0) });
        FAIL("expected recursion depth error");
    }
    catch (RuntimeError const& e)
    {
        REQUIRE(e.function() == "recursion");
        REQUIRE(e.thread_id() != 0);
        REQUIRE(std::string(e.what()).find("recursion") != std::string::npos);
        REQUIRE(vm.thread_status(ThreadRef{ e.thread_id() }) == ThreadStatus::failed);
    }

    vm.set_instruction_limit(20'000);
    try
    {
        (void)vm.call("spin");
        FAIL("expected infinite loop error");
    }
    catch (RuntimeError const& e)
    {
        REQUIRE(e.function() == "spin");
        REQUIRE(e.thread_id() != 0);
        REQUIRE(std::string(e.what()).find("infinite loop") != std::string::npos);
        REQUIRE(vm.thread_status(ThreadRef{ e.thread_id() }) == ThreadStatus::failed);
    }
}

TEST_CASE("t6 runtime errors carry frame context", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(fixture("basic.gsc"), "basic");

    try
    {
        (void)vm.call("divzero");
        FAIL("expected a RuntimeError");
    }
    catch (RuntimeError const& e)
    {
        REQUIRE(e.function() == "divzero");
        REQUIRE(e.opcode() == "OP_Divide");
        REQUIRE(e.module() == "basic.gsc");
        REQUIRE(e.thread_id() != 0);
    }

    auto const failed_thread = vm.start("basic", "divzero");
    REQUIRE_THROWS_AS(vm.run_until_idle(), RuntimeError);
    REQUIRE(vm.thread_status(failed_thread) == ThreadStatus::failed);

    REQUIRE_THROWS_AS(vm.call("does_not_exist"), Error);

    auto const garbage = std::vector<std::uint8_t>{ 1, 2, 3, 4, 5, 6, 7, 8 };
    REQUIRE_THROWS_AS(vm.load_module("garbage", garbage), Error);
}

TEST_CASE("t6 virtual files resolve includes and search paths", "[gscvm]")
{
    auto vm = make_vm();
    vm.add_search_path("scripts");
    vm.add_virtual_file("scripts/dep_helper.gsc", "helper_value() { return 77; }");
    vm.add_virtual_file("scripts/dep.gsc", "#include dep_helper;\nmain() { return helper_value(); }");

    REQUIRE(vm.resolve_virtual_path("dep_helper") == std::optional<std::string>{ "scripts/dep_helper.gsc" });
    REQUIRE(vm.resolve_virtual_path("scripts/dep.gsc") == std::optional<std::string>{ "scripts/dep.gsc" });
    REQUIRE(vm.read_virtual_file("dep_helper").has_value());
    REQUIRE(vm.virtual_files().size() == 2);

    vm.load_module_file("scripts/dep.gsc", "dep");
    vm.load_dependencies("dep");
    REQUIRE(vm.has_module("dep_helper"));
    REQUIRE(vm.call("dep", "main").as_integer() == 77);

    REQUIRE(vm.remove_virtual_file("scripts/dep_helper.gsc"));
    REQUIRE_FALSE(vm.resolve_virtual_path("dep_helper").has_value());
    vm.clear_virtual_files();
    REQUIRE(vm.virtual_files().empty());
}

TEST_CASE("t6 exposes thread, value and memory inspection", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("basic.gsc"), "basic");

    auto const args = std::array{ Value::integer(3), Value::integer(4) };
    REQUIRE(vm.set_breakpoint("basic", "add", 0));
    auto const thread = vm.start("basic", "add", args);

    REQUIRE(vm.run_until_idle() == 0);
    auto const paused = vm.debug_state();
    REQUIRE(paused.enabled);
    REQUIRE(paused.paused);
    REQUIRE(paused.thread == thread);
    REQUIRE(paused.function == "add");
    REQUIRE(paused.module == "basic.gsc");

    auto const info = vm.thread_info(thread);
    REQUIRE(info.status == ThreadStatus::paused);
    REQUIRE(info.frames.size() == 1);
    REQUIRE(info.frames[0].function == "add");
    REQUIRE(info.frames[0].locals.size() == 2);

    auto const before = vm.instruction_count();
    REQUIRE(vm.step() == 1);
    REQUIRE(vm.instruction_count() == before + 1);
    REQUIRE(vm.debug_state().paused);

    REQUIRE(vm.resume() > 0);
    REQUIRE(vm.thread_status(thread) == ThreadStatus::finished);
    REQUIRE(vm.thread_result(thread).as_integer() == 7);
    REQUIRE(vm.debug_state().paused == false);

    auto const globals = vm.globals();
    REQUIRE_FALSE(globals.empty());
    REQUIRE(globals[0].name == "level");

    auto const stats = vm.memory_stats();
    REQUIRE(stats.modules == 1);
    REQUIRE(stats.functions > 0);
    REQUIRE(stats.instructions > 0);
    REQUIRE(stats.module_bytes > 0);
    REQUIRE(stats.values > 0);

    auto const blocks = vm.memory_blocks(16);
    REQUIRE_FALSE(blocks.empty());
    REQUIRE(std::any_of(blocks.begin(), blocks.end(), [](auto const& block) { return block.kind == "module"; }));
    REQUIRE_FALSE(vm.module_bytes("basic").empty());

    auto array = Value::array();
    array.as_array().set(Value::integer(0), Value::integer(11));
    array.as_array().set(Value::integer(1), Value::string("nested"));
    auto const inspected = vm.inspect_value(array);
    REQUIRE(inspected.type == "array");
    REQUIRE(inspected.child_count == 2);
    REQUIRE(inspected.children.size() == 2);
    REQUIRE(inspected.children[0].value == "11");

    vm.clear_breakpoints();
    REQUIRE(vm.breakpoints().empty());
    REQUIRE_FALSE(vm.set_breakpoint("basic", "does_not_exist", 0));
}

TEST_CASE("t6 inspection expands nested object and array values", "[gscvm]")
{
    auto vm = make_vm();
    vm.load_module_file(source_fixture("objects.gsc"), "objects");

    auto const level = vm.call("objects", "level_debug");
    REQUIRE(level.is_object());

    auto const inspected = vm.inspect_value(level);
    REQUIRE(inspected.type == "object");
    REQUIRE(inspected.child_count >= 1);
    REQUIRE(std::any_of(inspected.children.begin(), inspected.children.end(), [](auto const& child) {
        return child.name == "test_value" && child.value == "5";
    }));

    auto const globals = vm.globals();
    REQUIRE_FALSE(globals.empty());
    REQUIRE(globals[0].child_count >= 1);

    auto const stats = vm.memory_stats();
    REQUIRE(stats.objects >= 1);
    REQUIRE(stats.values >= 1);
    REQUIRE(stats.nested_values >= 1);

    auto const blocks = vm.memory_blocks(8);
    REQUIRE(std::any_of(blocks.begin(), blocks.end(), [](auto const& block) { return block.kind == "object"; }));
}

} // namespace gsc::test
