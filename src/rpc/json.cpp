// SPDX-License-Identifier: MIT
// Oeeoo Coin — minimal JSON implementation.
#include "rpc/json.h"

#include <cmath>
#include <cstdio>

namespace oeeoo {

Json Json::Array() {
  Json j;
  j.type_ = Type::Array;
  return j;
}

Json Json::Object() {
  Json j;
  j.type_ = Type::Object;
  return j;
}

Json Json::IntFromString(const std::string& v) {
  Json j;
  try {
    j.type_ = Type::Int;
    j.int_ = std::stoll(v);
  } catch (...) {
    j.type_ = Type::Double;
    j.double_ = std::stod(v);
  }
  return j;
}

void Json::Push(Json value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

void Json::Set(const std::string& key, Json value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  object_[key] = std::move(value);
}

const Json* Json::Get(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}

namespace {
void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  Parser(const std::string& text) : text_(text) {}

  bool Run(Json& out, std::string& error) {
    SkipWs();
    if (!ParseValue(out, error)) return false;
    SkipWs();
    if (pos_ != text_.size()) {
      error = "trailing characters after JSON value";
      return false;
    }
    return true;
  }

 private:
  void SkipWs() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' ||
            text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool Expect(const char* word, std::string& error) {
    const std::size_t len = std::char_traits<char>::length(word);
    if (text_.compare(pos_, len, word) != 0) {
      error = std::string("expected '") + word + "'";
      return false;
    }
    pos_ += len;
    return true;
  }

  // Maximum nesting depth. Without a limit, a deeply nested payload
  // inside the 4 MB body cap can stack-overflow the parser (each
  // level costs several stack frames in ParseValue/ParseArray/
  // ParseObject). 64 is far above any legitimate RPC request.
  static constexpr int kMaxDepth = 64;

  bool ParseValue(Json& out, std::string& error, int depth = 0) {
    if (depth > kMaxDepth) {
      error = "JSON nesting too deep";
      return false;
    }
    if (pos_ >= text_.size()) {
      error = "unexpected end of input";
      return false;
    }
    const char c = text_[pos_];
    if (c == '{') return ParseObject(out, error, depth);
    if (c == '[') return ParseArray(out, error, depth);
    if (c == '"') {
      std::string s;
      if (!ParseString(s, error)) return false;
      out = Json(std::move(s));
      return true;
    }
    if (c == 't') {
      if (!Expect("true", error)) return false;
      out = Json(true);
      return true;
    }
    if (c == 'f') {
      if (!Expect("false", error)) return false;
      out = Json(false);
      return true;
    }
    if (c == 'n') {
      if (!Expect("null", error)) return false;
      out = Json();
      return true;
    }
    return ParseNumber(out, error);
  }

  bool ParseObject(Json& out, std::string& error, int depth) {
    ++pos_;  // {
    out = Json::Object();
    SkipWs();
    if (Consume('}')) return true;
    while (true) {
      SkipWs();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        error = "expected object key";
        return false;
      }
      std::string key;
      if (!ParseString(key, error)) return false;
      SkipWs();
      if (!Consume(':')) {
        error = "expected ':' in object";
        return false;
      }
      Json value;
      if (!ParseValue(value, error, depth + 1)) return false;
      out.Set(key, std::move(value));
      SkipWs();
      if (Consume('}')) return true;
      if (!Consume(',')) {
        error = "expected ',' or '}' in object";
        return false;
      }
    }
  }

  bool ParseArray(Json& out, std::string& error, int depth) {
    ++pos_;  // [
    out = Json::Array();
    SkipWs();
    if (Consume(']')) return true;
    while (true) {
      Json value;
      if (!ParseValue(value, error, depth + 1)) return false;
      out.Push(std::move(value));
      SkipWs();
      if (Consume(']')) return true;
      if (!Consume(',')) {
        error = "expected ',' or ']' in array";
        return false;
      }
    }
  }

  bool ParseHex4(uint32_t& cp, std::string& error) {
    if (pos_ + 4 > text_.size()) {
      error = "truncated \\u escape";
      return false;
    }
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        error = "bad \\u escape";
        return false;
      }
    }
    return true;
  }

  bool ParseString(std::string& out, std::string& error) {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') return true;
      if (c < 0x20) {
        error = "control character in string";
        return false;
      }
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (pos_ >= text_.size()) {
        error = "truncated escape";
        return false;
      }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!ParseHex4(cp, error)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: expect a low surrogate next.
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u') {
              error = "unpaired surrogate";
              return false;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!ParseHex4(low, error)) return false;
            if (low < 0xDC00 || low > 0xDFFF) {
              error = "bad low surrogate";
              return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            error = "unpaired surrogate";
            return false;
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          error = "bad escape";
          return false;
      }
    }
    error = "unterminated string";
    return false;
  }

  bool ParseNumber(Json& out, std::string& error) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size() ||
        text_[pos_] < '0' || text_[pos_] > '9') {
      error = "bad number";
      return false;
    }
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      while (pos_ < text_.size() && text_[pos_] >= '0' &&
             text_[pos_] <= '9') {
        ++pos_;
      }
    }
    bool isDouble = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      isDouble = true;
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' ||
          text_[pos_] > '9') {
        error = "bad fraction";
        return false;
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' &&
             text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      isDouble = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || text_[pos_] < '0' ||
          text_[pos_] > '9') {
        error = "bad exponent";
        return false;
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' &&
             text_[pos_] <= '9') {
        ++pos_;
      }
    }
    const std::string num = text_.substr(start, pos_ - start);
    if (isDouble) {
      try {
        out = Json(std::stod(num));
      } catch (...) {
        error = "number out of range";
        return false;
      }
    } else {
      try {
        out = Json(static_cast<int64_t>(std::stoll(num)));
      } catch (...) {
        error = "integer out of range";
        return false;
      }
    }
    return true;
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

void DumpString(const std::string& s, std::string& out) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void DumpValue(const Json& j, std::string& out) {
  switch (j.GetType()) {
    case Json::Type::Null:
      out += "null";
      break;
    case Json::Type::Bool:
      out += j.AsBool() ? "true" : "false";
      break;
    case Json::Type::Int:
      out += std::to_string(j.AsInt());
      break;
    case Json::Type::Double: {
      const double d = j.AsDouble();
      if (std::isnan(d) || std::isinf(d)) {
        out += "null";
        break;
      }
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g", d);
      out += buf;
      break;
    }
    case Json::Type::String:
      DumpString(j.AsString(), out);
      break;
    case Json::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const Json& item : j.AsArray()) {
        if (!first) out.push_back(',');
        first = false;
        DumpValue(item, out);
      }
      out.push_back(']');
      break;
    }
    case Json::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& kv : j.AsObject()) {
        if (!first) out.push_back(',');
        first = false;
        DumpString(kv.first, out);
        out.push_back(':');
        DumpValue(kv.second, out);
      }
      out.push_back('}');
      break;
    }
  }
}
}  // namespace

bool Json::Parse(const std::string& text, Json& out, std::string& error) {
  Parser parser(text);
  return parser.Run(out, error);
}

std::string Json::Dump() const {
  std::string out;
  DumpValue(*this, out);
  return out;
}

}  // namespace oeeoo
