// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "gsc/t6/format.hpp"

#include <cctype>

namespace gsc::t6
{

namespace
{

constexpr std::string_view ansi_colors[] = {
    "\x1b[30m", // ^0 black
    "\x1b[31m", // ^1 red
    "\x1b[32m", // ^2 green
    "\x1b[33m", // ^3 yellow
    "\x1b[34m", // ^4 blue
    "\x1b[36m", // ^5 cyan
    "\x1b[35m", // ^6 magenta
    "\x1b[0m",  // ^7 reset
};

} // namespace

auto format_message(std::string_view format, std::span<Value const> arguments) -> std::string
{
    auto output = std::string{};
    output.reserve(format.size());

    for (std::size_t i = 0; i < format.size();)
    {
        if (format[i] == '&' && i + 2 < format.size() && format[i + 1] == '&' && std::isdigit(static_cast<unsigned char>(format[i + 2])))
        {
            auto end = i + 2;
            auto index = std::size_t{ 0 };

            while (end < format.size() && std::isdigit(static_cast<unsigned char>(format[end])))
            {
                index = index * 10 + static_cast<std::size_t>(format[end] - '0');
                end++;
            }

            if (index >= 1 && index <= arguments.size())
            {
                output += arguments[index - 1].to_string();
                i = end;
                continue;
            }
        }

        output.push_back(format[i]);
        i++;
    }

    return apply_color_codes(output);
}

auto apply_color_codes(std::string_view text) -> std::string
{
    auto output = std::string{};
    output.reserve(text.size());

    auto colored = false;

    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] == '^' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7')
        {
            auto const color = static_cast<std::size_t>(text[i + 1] - '0');
            output += ansi_colors[color];
            colored = color != 7;
            i += 2;
            continue;
        }

        output.push_back(text[i]);
        i++;
    }

    if (colored)
        output += ansi_colors[7];

    return output;
}

} // namespace gsc::t6
