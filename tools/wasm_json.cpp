// Copyright 2026 gscvm contributors.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "wasm_json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <stdexcept>

#include "gsc/error.hpp"

namespace gsc::wasm
{

namespace
{

auto fail(std::string_view message) -> void
{
    throw gsc::Error(std::format("invalid JSON: {}", message));
}

auto append_utf8(std::string& out, std::uint32_t codepoint) -> void
{
    if (codepoint <= 0x7f)
    {
        out.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7ff)
    {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else if (codepoint <= 0xffff)
    {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else if (codepoint <= 0x10ffff)
    {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else
    {
        fail("invalid unicode escape");
    }
}

auto hex_value(char c) -> int
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text) {}

    auto parse() -> Json
    {
        skip_space();
        auto value = parse_value();
        skip_space();
        if (position_ != text_.size())
            fail("trailing characters");
        return value;
    }

private:
    auto skip_space() -> void
    {
        while (position_ < text_.size())
        {
            auto const c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            position_++;
        }
    }

    auto peek() const -> char
    {
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    auto take() -> char
    {
        if (position_ >= text_.size())
            fail("unexpected end of input");
        return text_[position_++];
    }

    auto expect(char expected) -> void
    {
        if (take() != expected)
            fail("unexpected character");
    }

    auto parse_value() -> Json
    {
        switch (peek())
        {
            case 'n': return parse_literal("null", Json{});
            case 't': return parse_literal("true", Json{ true });
            case 'f': return parse_literal("false", Json{ false });
            case '"': return Json{ parse_string() };
            case '[': return parse_array();
            case '{': return parse_object();
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                    return parse_number();
                fail("unexpected token");
        }
        return {};
    }

    auto parse_literal(std::string_view literal, Json value) -> Json
    {
        if (text_.substr(position_, literal.size()) != literal)
            fail("invalid literal");
        position_ += literal.size();
        return value;
    }

    auto parse_string() -> std::string
    {
        expect('"');
        auto out = std::string{};
        while (true)
        {
            auto const c = take();
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                fail("control character in string");
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }

            switch (take())
            {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    auto const parse_hex = [&]() {
                        auto value = 0u;
                        for (auto i = 0; i < 4; i++)
                        {
                            auto const digit = hex_value(take());
                            if (digit < 0)
                                fail("invalid unicode escape");
                            value = value * 16 + static_cast<std::uint32_t>(digit);
                        }
                        return value;
                    };

                    auto codepoint = parse_hex();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff)
                    {
                        if (take() != '\\' || take() != 'u')
                            fail("invalid surrogate pair");
                        auto const low = parse_hex();
                        if (low < 0xdc00 || low > 0xdfff)
                            fail("invalid surrogate pair");
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    }
                    append_utf8(out, codepoint);
                    break;
                }
                default:
                    fail("invalid escape");
            }
        }
    }

    auto parse_number() -> Json
    {
        auto const start = position_;
        if (peek() == '-')
            position_++;
        if (peek() == '0')
        {
            position_++;
        }
        else
        {
            if (peek() < '1' || peek() > '9')
                fail("invalid number");
            while (peek() >= '0' && peek() <= '9')
                position_++;
        }
        if (peek() == '.')
        {
            position_++;
            if (peek() < '0' || peek() > '9')
                fail("invalid number");
            while (peek() >= '0' && peek() <= '9')
                position_++;
        }
        if (peek() == 'e' || peek() == 'E')
        {
            position_++;
            if (peek() == '+' || peek() == '-')
                position_++;
            if (peek() < '0' || peek() > '9')
                fail("invalid number");
            while (peek() >= '0' && peek() <= '9')
                position_++;
        }

        auto const token = std::string{ text_.substr(start, position_ - start) };
        auto* end = static_cast<char*>(nullptr);
        auto const value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size())
            fail("invalid number");
        return Json{ value };
    }

    auto parse_array() -> Json
    {
        expect('[');
        auto out = Json::make_array();
        skip_space();
        if (peek() == ']')
        {
            position_++;
            return out;
        }
        while (true)
        {
            skip_space();
            out.array.push_back(parse_value());
            skip_space();
            auto const c = take();
            if (c == ']')
                return out;
            if (c != ',')
                fail("expected ',' or ']'");
        }
    }

    auto parse_object() -> Json
    {
        expect('{');
        auto out = Json::make_object();
        skip_space();
        if (peek() == '}')
        {
            position_++;
            return out;
        }
        while (true)
        {
            skip_space();
            if (peek() != '"')
                fail("expected object key");
            auto key = parse_string();
            skip_space();
            expect(':');
            skip_space();
            out.object.emplace_back(std::move(key), parse_value());
            skip_space();
            auto const c = take();
            if (c == '}')
                return out;
            if (c != ',')
                fail("expected ',' or '}'");
        }
    }

    std::string_view text_;
    std::size_t position_{};
};

auto append_escaped(std::string& out, std::string_view text) -> void
{
    out.push_back('"');
    for (auto const c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else
                    out.push_back(c);
                break;
        }
    }
    out.push_back('"');
}

auto append_number(std::string& out, double value) -> void
{
    if (!std::isfinite(value))
    {
        out += "null";
        return;
    }

    if (value == std::trunc(value) && std::abs(value) < 9007199254740992.0)
    {
        out += std::format("{:.0f}", value);
        return;
    }

    out += std::format("{}", value);
}

auto append_json(std::string& out, Json const& value) -> void
{
    switch (value.kind)
    {
        case Json::Kind::null: out += "null"; break;
        case Json::Kind::boolean: out += value.boolean ? "true" : "false"; break;
        case Json::Kind::number: append_number(out, value.number); break;
        case Json::Kind::string: append_escaped(out, value.string); break;
        case Json::Kind::array:
            out.push_back('[');
            for (std::size_t i = 0; i < value.array.size(); i++)
            {
                if (i != 0)
                    out.push_back(',');
                append_json(out, value.array[i]);
            }
            out.push_back(']');
            break;
        case Json::Kind::object:
            out.push_back('{');
            for (std::size_t i = 0; i < value.object.size(); i++)
            {
                if (i != 0)
                    out.push_back(',');
                append_escaped(out, value.object[i].first);
                out.push_back(':');
                append_json(out, value.object[i].second);
            }
            out.push_back('}');
            break;
    }
}

} // namespace

Json::Json(std::nullptr_t) : kind(Kind::null) {}
Json::Json(bool value) : kind(Kind::boolean), boolean(value) {}
Json::Json(double value) : kind(Kind::number), number(value) {}
Json::Json(int value) : kind(Kind::number), number(static_cast<double>(value)) {}
Json::Json(std::string value) : kind(Kind::string), string(std::move(value)) {}
Json::Json(char const* value) : kind(Kind::string), string(value ? value : "") {}
Json::Json(Array value) : kind(Kind::array), array(std::move(value)) {}
Json::Json(Object value) : kind(Kind::object), object(std::move(value)) {}

auto Json::null() -> Json { return Json{}; }
auto Json::make_array() -> Json { return Json{ Array{} }; }
auto Json::make_object() -> Json { return Json{ Object{} }; }

auto Json::is_null() const -> bool { return kind == Kind::null; }
auto Json::is_boolean() const -> bool { return kind == Kind::boolean; }
auto Json::is_number() const -> bool { return kind == Kind::number; }
auto Json::is_string() const -> bool { return kind == Kind::string; }
auto Json::is_array() const -> bool { return kind == Kind::array; }
auto Json::is_object() const -> bool { return kind == Kind::object; }

auto Json::find(std::string_view key) const -> Json const*
{
    if (kind != Kind::object)
        return nullptr;
    for (auto const& [name, value] : object)
    {
        if (name == key)
            return &value;
    }
    return nullptr;
}

auto Json::at(std::string_view key) const -> Json const&
{
    auto const* found = find(key);
    if (!found)
        throw gsc::Error(std::format("missing JSON field '{}'", key));
    return *found;
}

auto Json::append(Json value) -> Json&
{
    if (kind != Kind::array)
        throw gsc::Error("JSON value is not an array");
    array.push_back(std::move(value));
    return array.back();
}

auto Json::set(std::string key, Json value) -> Json&
{
    if (kind != Kind::object)
        throw gsc::Error("JSON value is not an object");
    for (auto& [name, existing] : object)
    {
        if (name == key)
        {
            existing = std::move(value);
            return existing;
        }
    }
    object.emplace_back(std::move(key), std::move(value));
    return object.back().second;
}

auto Json::as_bool() const -> bool
{
    if (!is_boolean())
        throw gsc::Error("JSON value is not a boolean");
    return boolean;
}

auto Json::as_number() const -> double
{
    if (!is_number())
        throw gsc::Error("JSON value is not a number");
    return number;
}

auto Json::as_string() const -> std::string const&
{
    if (!is_string())
        throw gsc::Error("JSON value is not a string");
    return string;
}

auto parse_json(std::string_view text) -> Json
{
    return Parser{ text }.parse();
}

auto stringify_json(Json const& value) -> std::string
{
    auto out = std::string{};
    append_json(out, value);
    return out;
}

auto json_quote(std::string_view text) -> std::string
{
    auto out = std::string{};
    append_escaped(out, text);
    return out;
}

} // namespace gsc::wasm
