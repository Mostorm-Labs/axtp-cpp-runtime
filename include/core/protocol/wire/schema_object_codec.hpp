#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "core/protocol/model/bytes.hpp"
#include "core/protocol/generated/schema_json_facts.hpp"

namespace axtp {

inline bool isCanonicalUint64Decimal(std::string_view value) noexcept {
    if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0')) return false;
    std::uint64_t result = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return true;
}

// Schema facts come exclusively from the generator. Semantic capability, owner,
// mutation and synchronization validation belongs to the consuming host.
class SchemaObjectCodec {
    using Json = nlohmann::json;
    static bool fieldValid(const Json& field, const Json& value, unsigned depth) {
        const auto type = field.at("type").get<std::string>();
        if (value.is_null()) return false;
        if (type == "string") {
            if (!value.is_string()) return false;
            const auto& text = value.get_ref<const std::string&>();
            if (field.contains("maxLength") && text.size() > field.at("maxLength").get<std::size_t>()) return false;
            if (field.value("canonicalUint64Decimal", false) && !isCanonicalUint64Decimal(text)) return false;
            try { (void)value.dump(); } catch (...) { return false; }
            return true;
        }
        if (type == "bool") return value.is_boolean();
        if (type == "enum") {
            if (!field.contains("enum")) return value.is_string() || value.is_number_integer();
            return std::find(field["enum"].begin(), field["enum"].end(), value) != field["enum"].end();
        }
        if (type.rfind("uint", 0) == 0 || type.rfind("int", 0) == 0) {
            if (!value.is_number_integer()) return false;
            const bool unsign = type.front() == 'u';
            const unsigned bits = static_cast<unsigned>(std::stoul(type.substr(unsign ? 4 : 3)));
            if (unsign) {
                if (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) return false;
                const auto n = value.get<std::uint64_t>();
                if (bits < 64 && n >= (std::uint64_t{1} << bits)) return false;
                if (field.contains("min") && n < field["min"].get<std::uint64_t>()) return false;
                if (field.contains("max") && n > field["max"].get<std::uint64_t>()) return false;
            } else {
                if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) return false;
                const auto n = value.get<std::int64_t>();
                if (bits < 64 && (n < -(std::int64_t{1} << (bits - 1)) || n >= (std::int64_t{1} << (bits - 1)))) return false;
                if (field.contains("min") && n < field["min"].get<std::int64_t>()) return false;
                if (field.contains("max") && n > field["max"].get<std::int64_t>()) return false;
            }
            return true;
        }
        if (type == "array") {
            if (!value.is_array() || !field.contains("array")) return false;
            auto item = Json{{"type", field["array"].value("itemSchema", field["array"].value("itemType", ""))}};
            for (const auto& v : value) if (!fieldValid(item, v, depth + 1)) return false;
            return true;
        }
        if (type == "object") {
            if (field.contains("schema")) return validate(field["schema"].get<std::string>(), value, depth + 1);
            return value.is_object();
        }
        if (type == "bytes") return value.is_string() || value.is_binary();
        if (type == "bitmap") return value.is_string() || value.is_array() || value.is_number_unsigned();
        return validate(type, value, depth + 1);
    }
public:
    static bool validate(std::string_view schema, const Json& object, unsigned depth = 0) {
        if (depth > 32 || !object.is_object()) return false;
        const auto& facts = generated::schemaJsonFacts();
        const auto found = facts.find(std::string(schema));
        if (found == facts.end()) return false;
        try {
            for (const auto& field : found->at("fields")) {
                const auto key = field.at("name").get<std::string>();
                if (!object.contains(key)) { if (field.value("required", false)) return false; }
                else {
                    auto selected = field;
                    if (field.contains("variants")) {
                        const auto discriminator = field["variants"].at("discriminator").get<std::string>();
                        if (!object.contains(discriminator) || !object[discriminator].is_string()) return false;
                        const auto choice = object[discriminator].get<std::string>();
                        const auto& mapping = field["variants"].at("mapping");
                        if (!mapping.contains(choice)) return false;
                        selected["schema"] = mapping.at(choice);
                    }
                    if (!fieldValid(selected, object.at(key), depth)) return false;
                }
            }
            (void)object.dump();
            return true;
        } catch (...) { return false; }
    }
    static std::optional<Json> decodeJson(std::string_view schema, std::string_view text) {
        try { auto v = Json::parse(text); if (validate(schema, v)) return v; } catch (...) {}
        return std::nullopt;
    }
    static std::optional<std::string> encodeJson(std::string_view schema, const Json& object) {
        if (!validate(schema, object)) return std::nullopt;
        return object.dump();
    }
    // TLV8 object/scalar projection. Arrays/enum layouts without an explicit
    // repeated/packed/enum-width binding are rejected, never guessed.
    static std::optional<Bytes> encodeTlv8(std::string_view schema, const Json& object, unsigned depth = 0) {
        if (depth > 32 || !validate(schema, object)) return std::nullopt;
        auto fields = generated::schemaJsonFacts().at(std::string(schema)).at("fields");
        std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) {return a.at("id") < b.at("id");});
        Bytes result;
        for (const auto& field : fields) {
            const auto key = field.at("name").template get<std::string>();
            if (!object.contains(key)) continue;
            const auto type = field.at("type").template get<std::string>();
            const auto id = field.at("id").template get<unsigned>();
            const auto& value = object.at(key);
            Bytes data;
            if (type == "string") {const auto s=value.template get<std::string>();data.assign(s.begin(),s.end());}
            else if (type == "bool") data.push_back(value.template get<bool>() ? 1 : 0);
            else if (type.rfind("uint",0)==0) {
                const unsigned size=static_cast<unsigned>(std::stoul(type.substr(4)))/8;
                const auto n=value.template get<std::uint64_t>();
                for (unsigned i=size;i>0;--i) data.push_back(static_cast<Byte>(n >> ((i-1)*8)));
            } else if (generated::schemaJsonFacts().contains(type)) {
                auto nested=encodeTlv8(type,value,depth+1);if(!nested)return std::nullopt;data=std::move(*nested);
            } else return std::nullopt;
            if (id==0 || id>=255 || data.size()>255) return std::nullopt;
            result.push_back(static_cast<Byte>(id));result.push_back(static_cast<Byte>(data.size()));
            result.insert(result.end(),data.begin(),data.end());
        }
        return result;
    }
    static std::optional<Json> decodeTlv8(std::string_view schema, const Bytes& data, unsigned depth=0) {
        const auto& facts=generated::schemaJsonFacts();
        if(depth>32 || !facts.contains(std::string(schema)))return std::nullopt;
        const auto& fields=facts.at(std::string(schema)).at("fields");
        Json object=Json::object();
        for(std::size_t at=0;at<data.size();) {
            if(data.size()-at<2)return std::nullopt;
            const auto id=data[at++];const auto size=data[at++];
            if(size>data.size()-at)return std::nullopt;
            auto field=std::find_if(fields.begin(),fields.end(),[id](const auto& f){return f.at("id")==id;});
            if(field!=fields.end()) {
                const auto key=field->at("name").template get<std::string>();
                if(object.contains(key))return std::nullopt;
                const auto type=field->at("type").template get<std::string>();
                if(type=="string")object[key]=std::string(data.begin()+at,data.begin()+at+size);
                else if(type=="bool") {if(size!=1 || data[at]>1)return std::nullopt;object[key]=data[at]==1;}
                else if(type.rfind("uint",0)==0) {
                    const auto width=std::stoul(type.substr(4))/8;if(size!=width)return std::nullopt;
                    std::uint64_t n=0;for(std::size_t i=0;i<size;++i)n=(n<<8)|data[at+i];object[key]=n;
                } else if(facts.contains(type)) {
                    auto nested=decodeTlv8(type,Bytes(data.begin()+at,data.begin()+at+size),depth+1);
                    if(!nested)return std::nullopt;object[key]=*nested;
                } else return std::nullopt;
            }
            at+=size;
        }
        if(!validate(schema,object))return std::nullopt;
        return object;
    }
};
} // namespace axtp
