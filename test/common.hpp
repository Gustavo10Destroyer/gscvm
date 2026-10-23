// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gsc/gscvm.hpp"

namespace gsc::test
{

inline auto fixture(std::string_view name) -> std::filesystem::path
{
    return std::filesystem::path{ GSCVM_TEST_DATA } / "t6" / name;
}

inline auto source_fixture(std::string_view name) -> std::filesystem::path
{
    return std::filesystem::path{ GSCVM_TEST_DATA }.parent_path() / "fixtures" / "t6" / name;
}

inline auto read_bytes(std::filesystem::path const& path) -> std::vector<std::uint8_t>
{
    auto stream = std::ifstream{ path, std::ios::binary };
    if (!stream)
        throw gsc::Error("could not open " + path.string());

    stream.seekg(0, std::ios::end);
    auto const length = stream.tellg();
    stream.seekg(0, std::ios::beg);

    auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

inline auto make_vm(gsc::t6::VMConfig config = {}) -> gsc::t6::VM
{
    // Tests use deterministic virtual time unless they explicitly override it.
    config.real_time = false;
    if (!config.host)
        config.host = std::make_shared<gsc::t6::DefaultHost>();

    auto vm = gsc::t6::VM{ config };
    vm.natives().register_function("", "println", [](gsc::t6::NativeContext&) {});
    vm.natives().register_function("", "print", [](gsc::t6::NativeContext&) {});
    return vm;
}

} // namespace gsc::test
