#pragma once

// Mini-parser JSON compartido (lectura por clave y escapeo para escritura).
// Sólo lo que el framework persiste: objetos planos, arrays y escalares.

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace survival_shield {

inline void SkipWhitespace(std::string_view json, std::size_t& position) noexcept {
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
}

inline bool SkipJsonString(std::string_view json, std::size_t& position) noexcept {
    if (position >= json.size() || json[position] != '"') {
        return false;
    }

    ++position;
    while (position < json.size()) {
        const char current = json[position++];
        if (current == '"') {
            return true;
        }
        if (current == '\\') {
            if (position >= json.size()) {
                return false;
            }
            ++position;
        }
    }
    return false;
}

inline bool SkipJsonValue(std::string_view json, std::size_t& position) noexcept {
    if (position >= json.size()) {
        return false;
    }

    if (json[position] == '"') {
        return SkipJsonString(json, position);
    }

    if (json[position] == '{' || json[position] == '[') {
        const char closing = json[position] == '{' ? '}' : ']';
        ++position;
        SkipWhitespace(json, position);
        if (position < json.size() && json[position] == closing) {
            ++position;
            return true;
        }

        while (position < json.size()) {
            if (closing == '}') {
                if (!SkipJsonString(json, position)) {
                    return false;
                }
                SkipWhitespace(json, position);
                if (position >= json.size() || json[position++] != ':') {
                    return false;
                }
                SkipWhitespace(json, position);
                if (!SkipJsonValue(json, position)) {
                    return false;
                }
            } else if (!SkipJsonValue(json, position)) {
                return false;
            }

            SkipWhitespace(json, position);
            if (position >= json.size()) {
                return false;
            }
            if (json[position] == closing) {
                ++position;
                return true;
            }
            if (json[position++] != ',') {
                return false;
            }
            SkipWhitespace(json, position);
        }
        return false;
    }

    const std::size_t start = position;
    while (position < json.size() && json[position] != ',' && json[position] != '}' &&
           json[position] != ']' && json[position] != ' ' && json[position] != '\t' &&
           json[position] != '\r' && json[position] != '\n') {
        ++position;
    }
    return position > start;
}

inline std::optional<std::string_view> FindJsonValue(std::string_view json,
                                                     std::string_view key) {
    std::size_t position = 0;
    SkipWhitespace(json, position);
    if (position >= json.size() || json[position++] != '{') {
        return std::nullopt;
    }

    SkipWhitespace(json, position);
    while (position < json.size() && json[position] != '}') {
        if (json[position] != '"') {
            return std::nullopt;
        }
        const std::size_t keyStart = ++position;
        if (!SkipJsonString(json, --position)) {
            return std::nullopt;
        }
        const std::size_t keyEnd = position - 1U;
        const bool matches = json.substr(keyStart, keyEnd - keyStart) == key;

        SkipWhitespace(json, position);
        if (position >= json.size() || json[position++] != ':') {
            return std::nullopt;
        }
        SkipWhitespace(json, position);
        const std::size_t valueStart = position;
        if (!SkipJsonValue(json, position)) {
            return std::nullopt;
        }
        const std::size_t valueEnd = position;
        if (matches) {
            return json.substr(valueStart, valueEnd - valueStart);
        }

        SkipWhitespace(json, position);
        if (position >= json.size() || json[position++] != ',') {
            return std::nullopt;
        }
        SkipWhitespace(json, position);
    }
    return std::nullopt;
}

inline std::string DecodeJsonString(std::string_view raw) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        return {};
    }

    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        char current = raw[i];
        if (current != '\\') {
            out.push_back(current);
            continue;
        }

        ++i;
        if (i + 1 >= raw.size()) {
            break;
        }
        current = raw[i];
        switch (current) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            if (i + 4 >= raw.size()) {
                break;
            }
            unsigned int code = 0;
            const auto result = std::from_chars(raw.data() + i + 1, raw.data() + i + 5, code, 16);
            if (result.ec != std::errc{}) {
                break;
            }
            i += 4;
            if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            break;
        }
        default: break;
        }
    }
    return out;
}

inline std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char current : value) {
        const unsigned char byte = static_cast<unsigned char>(current);
        switch (current) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20) {
                char escape[7]{};
                std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
                out += escape;
            } else {
                out.push_back(current);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

} // namespace survival_shield
