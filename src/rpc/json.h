// SPDX-License-Identifier: MIT
// Oeeoo Coin — minimal JSON (Milestone 10).
// A small, dependency-free JSON value, parser and serializer for the
// RPC and Stratum servers. It covers the JSON-RPC subset: null,
// booleans, integers (int64), doubles, strings (UTF-8), arrays and
// objects. Numbers without a fraction/exponent parse as integers;
// anything else parses as a double. The parser is strict: no
// trailing garbage, no leading zeros, no bare control characters in
// strings, and \u escapes (including surrogate pairs) are decoded
// to UTF-8. This is not a general-purpose JSON library — it is the
// smallest correct tool for this node's RPC surface.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oeeoo {

class Json {
 public:
  enum class Type { Null, Bool, Int, Double, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool v) : type_(Type::Bool), bool_(v) {}
  Json(int v) : type_(Type::Int), int_(v) {}
  Json(int64_t v) : type_(Type::Int), int_(v) {}
  Json(uint64_t v) : type_(Type::Int), int_(static_cast<int64_t>(v)) {}
  Json(double v) : type_(Type::Double), double_(v) {}
  Json(const char* v) : type_(Type::String), string_(v) {}
  Json(const std::string& v) : type_(Type::String), string_(v) {}
  Json(std::string&& v) : type_(Type::String), string_(std::move(v)) {}

  static Json Array();
  static Json Object();
  static Json IntFromString(const std::string& v);

  Type GetType() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsBool() const { return type_ == Type::Bool; }
  bool IsInt() const { return type_ == Type::Int; }
  bool IsDouble() const { return type_ == Type::Double; }
  bool IsNumber() const {
    return type_ == Type::Int || type_ == Type::Double;
  }
  bool IsString() const { return type_ == Type::String; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  bool AsBool(bool fallback = false) const {
    return type_ == Type::Bool ? bool_ : fallback;
  }
  int64_t AsInt(int64_t fallback = 0) const {
    if (type_ == Type::Int) return int_;
    if (type_ == Type::Double) return static_cast<int64_t>(double_);
    return fallback;
  }
  double AsDouble(double fallback = 0.0) const {
    if (type_ == Type::Double) return double_;
    if (type_ == Type::Int) return static_cast<double>(int_);
    return fallback;
  }
  const std::string& AsString() const { return string_; }
  const std::vector<Json>& AsArray() const { return array_; }
  const std::map<std::string, Json>& AsObject() const { return object_; }

  void Push(Json value);
  void Set(const std::string& key, Json value);
  // nullptr when the key is absent or this is not an object.
  const Json* Get(const std::string& key) const;
  bool Has(const std::string& key) const { return Get(key) != nullptr; }

  static bool Parse(const std::string& text, Json& out,
                    std::string& error);
  std::string Dump() const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  int64_t int_ = 0;
  double double_ = 0.0;
  std::string string_;
  std::vector<Json> array_;
  std::map<std::string, Json> object_;
};

}  // namespace oeeoo
