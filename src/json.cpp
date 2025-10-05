#include "json.hpp"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace me {

namespace {

void dump_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void dump_value(const Json& j, std::string& out) {
    switch (j.type) {
        case Json::Type::Null:
            out += "null";
            break;
        case Json::Type::Bool:
            out += j.bool_value ? "true" : "false";
            break;
        case Json::Type::Int:
            out += std::to_string(j.int_value);
            break;
        case Json::Type::Double: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", j.double_value);
            out += buf;
            break;
        }
        case Json::Type::String:
            dump_string(j.string_value, out);
            break;
        case Json::Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < j.array_value.size(); ++i) {
                if (i) out.push_back(',');
                dump_value(j.array_value[i], out);
            }
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            out.push_back('{');
            for (size_t i = 0; i < j.object_value.size(); ++i) {
                if (i) out.push_back(',');
                dump_string(j.object_value[i].first, out);
                out.push_back(':');
                dump_value(j.object_value[i].second, out);
            }
            out.push_back('}');
            break;
        }
    }
}

struct Parser {
    const std::string& text;
    size_t pos = 0;

    explicit Parser(const std::string& t) : text(t) {}

    [[noreturn]] void fail(const std::string& msg) { throw std::runtime_error("json parse error: " + msg); }

    void skip_ws() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    char peek() {
        if (pos >= text.size()) fail("unexpected end");
        return text[pos];
    }

    Json parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string_value();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        return parse_number();
    }

    Json parse_object() {
        Json obj = Json::make_object();
        ++pos;  // consume {
        skip_ws();
        if (peek() == '}') {
            ++pos;
            return obj;
        }
        while (true) {
            skip_ws();
            if (peek() != '"') fail("expected object key");
            std::string key = parse_raw_string();
            skip_ws();
            if (peek() != ':') fail("expected ':'");
            ++pos;
            Json value = parse_value();
            obj.object_value.emplace_back(std::move(key), std::move(value));
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++pos;
                continue;
            }
            if (c == '}') {
                ++pos;
                break;
            }
            fail("expected ',' or '}'");
        }
        return obj;
    }

    Json parse_array() {
        Json arr = Json::make_array();
        ++pos;  // consume [
        skip_ws();
        if (peek() == ']') {
            ++pos;
            return arr;
        }
        while (true) {
            Json value = parse_value();
            arr.array_value.push_back(std::move(value));
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++pos;
                continue;
            }
            if (c == ']') {
                ++pos;
                break;
            }
            fail("expected ',' or ']'");
        }
        return arr;
    }

    std::string parse_raw_string() {
        ++pos;  // consume opening quote
        std::string out;
        while (true) {
            if (pos >= text.size()) fail("unterminated string");
            char c = text[pos++];
            if (c == '"') break;
            if (c == '\\') {
                if (pos >= text.size()) fail("bad escape");
                char e = text[pos++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (pos + 4 > text.size()) fail("bad unicode escape");
                        int code = std::stoi(text.substr(pos, 4), nullptr, 16);
                        pos += 4;
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
                    default: fail("unknown escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Json parse_string_value() {
        return Json::from_string(parse_raw_string());
    }

    Json parse_bool() {
        if (text.compare(pos, 4, "true") == 0) {
            pos += 4;
            Json j;
            j.type = Json::Type::Bool;
            j.bool_value = true;
            return j;
        }
        if (text.compare(pos, 5, "false") == 0) {
            pos += 5;
            Json j;
            j.type = Json::Type::Bool;
            j.bool_value = false;
            return j;
        }
        fail("invalid literal");
    }

    Json parse_null() {
        if (text.compare(pos, 4, "null") == 0) {
            pos += 4;
            return Json();
        }
        fail("invalid literal");
    }

    Json parse_number() {
        size_t start = pos;
        bool is_double = false;
        if (peek() == '-') ++pos;
        while (pos < text.size()) {
            char c = text[pos];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                ++pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_double = true;
                ++pos;
            } else {
                break;
            }
        }
        std::string token = text.substr(start, pos - start);
        if (token.empty()) fail("invalid number");
        Json j;
        if (is_double) {
            j.type = Json::Type::Double;
            j.double_value = std::stod(token);
        } else {
            j.type = Json::Type::Int;
            j.int_value = std::stoll(token);
        }
        return j;
    }
};

}  // namespace

std::string Json::dump() const {
    std::string out;
    dump_value(*this, out);
    return out;
}

Json Json::parse(const std::string& text) {
    Parser parser(text);
    Json value = parser.parse_value();
    parser.skip_ws();
    return value;
}

}  // namespace me
