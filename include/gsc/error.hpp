// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace gsc
{

// Base class for every error raised by the library. Dialect implementations add
// their own diagnostic context on top of this.
class Error : public std::runtime_error
{
public:
    explicit Error(std::string message) : std::runtime_error(std::move(message)) {}
};

} // namespace gsc
