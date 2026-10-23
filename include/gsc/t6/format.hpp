// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "gsc/t6/value.hpp"

namespace gsc::t6
{

// Implements the engine's print formatting:
// - "&&1", "&&2", ... are replaced by the corresponding argument,
// - "^0".."^7" become ANSI colours, with "^7" resetting the colour.
// If a colour is still active at the end, an ANSI reset is appended so terminal
// state never leaks into the next line.
auto format_message(std::string_view format, std::span<Value const> arguments) -> std::string;

// Applies engine colour codes to already-formatted text.
auto apply_color_codes(std::string_view text) -> std::string;

} // namespace gsc::t6
