// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/compiler.hpp"

#include <algorithm>
#include <fstream>
#include <format>
#include <utility>

#include "xsk/arc/engine/t6_pc.hpp"

namespace gsc::t6::detail
{

namespace
{

auto normalize(std::string_view path) -> std::string
{
    auto result = std::string{ path };
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

auto read_file(std::filesystem::path const& path, std::vector<std::uint8_t>& out) -> bool
{
    auto stream = std::ifstream{ path, std::ios::binary };
    if (!stream)
        return false;

    stream.seekg(0, std::ios::end);
    auto const length = stream.tellg();
    if (length <= 0)
        return false;
    stream.seekg(0, std::ios::beg);

    out.resize(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(stream);
}

} // namespace

auto is_compiled(std::span<std::uint8_t const> data) -> bool
{
    return data.size() >= 4 && data[0] == 0x80 && data[1] == 'G' && data[2] == 'S' && data[3] == 'C';
}

auto compile_source(std::string const& name, std::string_view source, CompileOptions const& options) -> std::vector<std::uint8_t>
{
    if (source.empty())
        throw Error(std::format("cannot compile empty script '{}'", name));

    auto file_name = name;
    if (std::filesystem::path{ file_name }.extension().empty())
        file_name += options.instance == Instance::client ? ".csc" : ".gsc";

    auto context = xsk::arc::t6::pc::context{ options.instance == Instance::client ? xsk::arc::instance::client : xsk::arc::instance::server };
    auto const search_paths = options.search_paths;
    auto const virtual_loader = options.virtual_loader;

    context.init(options.dev_blocks ? xsk::arc::build::dev_blocks : xsk::arc::build::prod,
        [search_paths, virtual_loader](std::string const& requested) -> std::vector<std::uint8_t> {
            auto const normalized = normalize(requested);
            auto data = std::vector<std::uint8_t>{};

            if (virtual_loader)
            {
                data = virtual_loader(normalized);
                if (!data.empty())
                    return data;
            }

            for (auto const& path : search_paths)
            {
                if (read_file(path / normalized, data))
                    return data;
            }

            if (read_file(normalized, data))
                return data;

            return {};
        });

    try
    {
        auto data = std::vector<std::uint8_t>{ source.begin(), source.end() };
        auto assembly = context.compiler().compile(file_name, data);
        auto binary = context.assembler().assemble(*assembly, std::filesystem::path{ file_name }.filename().string());

        if (binary.first.size == 0 || binary.first.data == nullptr)
            throw Error(std::format("compiler produced no bytecode for '{}'", name));

        return { binary.first.data, binary.first.data + binary.first.size };
    }
    catch (Error const&)
    {
        throw;
    }
    catch (std::exception const& e)
    {
        throw Error(std::format("failed to compile '{}': {}", name, e.what()));
    }
}

} // namespace gsc::t6::detail
