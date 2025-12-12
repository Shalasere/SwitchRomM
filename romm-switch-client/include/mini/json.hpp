#pragma once
// Extremely small JSON helper for key/value configs (strings + ints).
// This is not a full JSON implementation; it accepts simple objects with
// string keys and string/int/bool values. Good enough for config.json shape.

#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdint>
#include <vector>

namespace mini {

struct Value;
using Object = std::unordered_map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    enum class Type { String, Number, Bool, Null, Object, Array } type{Type::Null};
    std::string str;
    int64_t number{0};
    bool boolean{false};
    Object object;
    Array array;
};

inline void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
}

inline bool parse_string(const std::string& s, size_t& i, std::string& out) {
    if (s[i] != '"') return false;
    i++; out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '\\' && i < s.size()) {
            char esc = s[i++];
            if (esc == 'n') out.push_back('\n');
            else out.push_back(esc);
        } else if (c == '"') {
            return true;
        } else {
            out.push_back(c);
        }
    }
    return false;
}

inline bool parse_object(const std::string& s, size_t& i, Object& out); // fwd
inline bool parse_array(const std::string& s, size_t& i, Array& out);

inline bool parse_value(const std::string& s, size_t& i, Value& out) {
    skip_ws(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '"') {
        out.type = Value::Type::String;
        return parse_string(s, i, out.str);
    }
    if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-') {
        size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if (std::isdigit(static_cast<unsigned char>(c)) || c=='-' || c=='+' || c=='.' || c=='e' || c=='E') {
                i++;
            } else break;
        }
        out.type = Value::Type::Number;
        out.number = std::strtoll(s.substr(start, i - start).c_str(), nullptr, 10);
        return true;
    }
    if (s.compare(i, 4, "true") == 0) {
        out.type = Value::Type::Bool; out.boolean = true; i += 4; return true;
    }
    if (s.compare(i, 5, "false") == 0) {
        out.type = Value::Type::Bool; out.boolean = false; i += 5; return true;
    }
    if (s.compare(i, 4, "null") == 0) {
        out.type = Value::Type::Null; i += 4; return true;
    }
    if (s[i] == '{') {
        out.type = Value::Type::Object;
        return parse_object(s, i, out.object);
    }
    if (s[i] == '[') {
        out.type = Value::Type::Array;
        return parse_array(s, i, out.array);
    }
    return false;
}

inline bool parse_object(const std::string& s, size_t& i, Object& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    i++;
    skip_ws(s, i);
    while (i < s.size() && s[i] != '}') {
        std::string key;
        if (!parse_string(s, i, key)) return false;
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        i++;
        Value v;
        if (!parse_value(s, i, v)) return false;
        out[key] = v;
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { i++; skip_ws(s, i); }
    }
    if (i < s.size() && s[i] == '}') { i++; return true; }
    return false;
}

inline bool parse_array(const std::string& s, size_t& i, Array& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '[') return false;
    i++;
    skip_ws(s, i);
    while (i < s.size() && s[i] != ']') {
        Value v;
        if (!parse_value(s, i, v)) return false;
        out.push_back(std::move(v));
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { i++; skip_ws(s, i); }
    }
    if (i < s.size() && s[i] == ']') { i++; return true; }
    return false;
}

inline bool parse(const std::string& s, Object& out) {
    size_t i = 0;
    return parse_object(s, i, out);
}

inline bool parse(const std::string& s, Array& out) {
    size_t i = 0;
    return parse_array(s, i, out);
}

} // namespace mini
