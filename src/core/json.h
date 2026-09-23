#pragma once
// Minimal JSON value type: parser + serializer for content data, scenario files
// and human-readable debug dumps. Deliberately dependency-free.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hoi {

class Json {
  public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), num_(b ? 1 : 0) {}
    Json(double d) : type_(Type::Number), num_(d) {}
    Json(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(uint32_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json object();
    static Json array();

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool is_null() const { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const { return type_ == Type::Object; }

    // Object access. Missing key yields a null Json.
    [[nodiscard]] bool has(const std::string& key) const;
    [[nodiscard]] const Json& at(const std::string& key) const;
    const Json& operator[](const std::string& key) const { return at(key); }
    // Array access. Out-of-range yields a null Json.
    [[nodiscard]] const Json& at(size_t index) const;
    const Json& operator[](size_t index) const { return at(index); }
    [[nodiscard]] size_t size() const;

    [[nodiscard]] double as_double(double fallback = 0.0) const;
    [[nodiscard]] int64_t as_int(int64_t fallback = 0) const;
    [[nodiscard]] bool as_bool(bool fallback = false) const;
    [[nodiscard]] std::string as_string(const std::string& fallback = "") const;

    // Mutation / construction.
    void set(const std::string& key, Json value);
    void push_back(Json value);

    [[nodiscard]] const std::vector<std::pair<std::string, Json>>& object_items() const {
        return obj_;
    }
    [[nodiscard]] const std::vector<Json>& array_items() const { return arr_; }

    // Parses `text`. On failure returns a null Json and fills `err`.
    static Json parse(const std::string& text, std::string* err = nullptr);
    static bool parse_file(const std::string& path, Json* out, std::string* err = nullptr);

    [[nodiscard]] std::string dump(int indent = -1) const;
    bool write_file(const std::string& path, int indent = -1) const;

  private:
    Type type_ = Type::Null;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace hoi
