// json.h - a small JSON value: enough for the payload's files, the schema, manifest.json and
// atmt_install.json. Objects keep their key order, so a file the manager rewrites stays diffable.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace atmt {

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Bool), bool_(b) {}
    Json(int v) : type_(Number), num_(v) {}
    Json(int64_t v) : type_(Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Number), num_(v) {}
    Json(const char* s) : type_(String), str_(s) {}
    Json(std::string s) : type_(String), str_(std::move(s)) {}

    static Json MakeArray() { Json j; j.type_ = Array; return j; }
    static Json MakeObject() { Json j; j.type_ = Object; return j; }

    // Parses `text`; on failure returns false and says where in `error`.
    static bool Parse(const std::string& text, Json* out, std::string* error = nullptr);
    std::string Dump(bool pretty = true) const;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Null; }
    bool is_object() const { return type_ == Object; }
    bool is_array() const { return type_ == Array; }
    bool is_string() const { return type_ == String; }

    // Typed reads with a fallback for a missing key or a value of another type.
    bool AsBool(bool def = false) const { return type_ == Bool ? bool_ : def; }
    double AsNumber(double def = 0) const { return type_ == Number ? num_ : def; }
    int64_t AsInt(int64_t def = 0) const { return type_ == Number ? static_cast<int64_t>(num_) : def; }
    std::string AsString(const std::string& def = std::string()) const { return type_ == String ? str_ : def; }

    // Object access. operator[] on a const object returns a null Json for a missing key.
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);   // inserts (turns a null into an object)
    bool Has(const std::string& key) const;
    void Erase(const std::string& key);
    const std::vector<std::pair<std::string, Json>>& items() const { return obj_; }

    // Array access.
    const std::vector<Json>& elements() const { return arr_; }
    std::vector<Json>& elements() { return arr_; }
    void Push(Json v);
    size_t size() const { return type_ == Array ? arr_.size() : type_ == Object ? obj_.size() : 0; }

    // Convenience: obj["a"]["b"] as a string, with a default.
    std::string Str(const std::string& key, const std::string& def = std::string()) const {
        return (*this)[key].AsString(def);
    }

private:
    void DumpTo(std::string& out, bool pretty, int indent) const;

    Type type_ = Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace atmt
