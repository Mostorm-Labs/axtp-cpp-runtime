#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "core/protocol/wire/schema_object_codec.hpp"
#include "core/protocol/generated/method_registry.h"

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
axtp::Bytes fromHex(const std::string& hex) {
    axtp::Bytes bytes;
    for (std::size_t i=0;i<hex.size();i+=2) bytes.push_back(static_cast<axtp::Byte>(std::stoul(hex.substr(i,2),nullptr,16)));
    return bytes;
}
}

int main() {
    try {
        const char* path=std::getenv("AXTP_SPEC_PATH");
        require(path!=nullptr,"AXTP_SPEC_PATH is required for exact frozen vectors");
        std::size_t count=0;
        for(const auto* name:{"cast_control_encoding.json","cast_settings_encoding.json"}) {
            std::ifstream file(std::string(path)+"/contract/test-vectors/"+name);
            nlohmann::json vectors; file >> vectors;
            const auto schema=vectors.at("schema").get<std::string>();
            for(const auto& pair:vectors.at("pairs")) {
                const bool expected=pair.at("accepted");
                const auto& value=pair.at("json");
                const auto body=fromHex(pair.at("tlv8Hex"));
                require(axtp::SchemaObjectCodec::validate(schema,value)==expected,pair.at("name"));
                const auto decoded=axtp::SchemaObjectCodec::decodeTlv8(schema,body);
                require(decoded.has_value()==expected,"TLV rejection "+pair.at("name").get<std::string>());
                if(expected) {
                    require(*decoded==value,"TLV exact string roundtrip");
                    require(axtp::SchemaObjectCodec::encodeTlv8(schema,value)==body,"TLV canonical encoding");
                    auto future=value;future["futureOptional"]={{"x",true}};
                    require(axtp::SchemaObjectCodec::decodeJson(schema,future.dump()).has_value(),"unknown JSON optional");
                    auto extra=body;extra.insert(extra.end(),{0xFE,2,0xAA,0xBB});
                    require(axtp::SchemaObjectCodec::decodeTlv8(schema,extra)==decoded,"unknown TLV optional");
                    extra.push_back(0xFD);
                    require(!axtp::SchemaObjectCodec::decodeTlv8(schema,extra),"truncated unknown TLV");
                }
                ++count;
            }
        }
        require(axtp::MethodRegistry::fromGeneratedDefaults().findMethodId("cast.setPresentationSettings")==0x1619,"generated P1 method");
        require(axtp::MethodRegistry::fromGeneratedDefaults().findMethodId("cast.getSession")==0x1601,"legacy method unchanged");
        const nlohmann::json context={{"controlVersion",1},{"runtimeInstanceId","11111111-1111-4111-8111-111111111111"},
            {"backendRevision","9007199254740993"},{"backendEventSequence","18446744073709551615"}};
        const auto wire=axtp::SchemaObjectCodec::encodeTlv8("CastWebCastBackendContext",context);
        require(wire.has_value(),"generated backend context TLV encoding");
        require(axtp::SchemaObjectCodec::decodeTlv8("CastWebCastBackendContext",*wire)==context,"backend exact uint64 string roundtrip");
        auto future=context;future["unknownOptional"]=true;
        require(axtp::SchemaObjectCodec::decodeJson("CastWebCastBackendContext",future.dump()).has_value(),"backend future optional JSON");
        auto extra=*wire;extra.insert(extra.end(),{0xFE,1,0x01});
        require(axtp::SchemaObjectCodec::decodeTlv8("CastWebCastBackendContext",extra)==context,"backend future optional TLV");
        for(const auto& bad:nlohmann::json::array({1,"01","-1","18446744073709551616"})) {
            future=context;future["backendRevision"]=bad;
            require(!axtp::SchemaObjectCodec::validate("CastWebCastBackendContext",future),"backend invalid decimal counter rejected");
        }
        require(axtp::MethodRegistry::fromGeneratedDefaults().findMethodId("cast.getWebCastBackendState")==0x161A,"new generated backend method");
        std::cout << "cast object codec vectors passed: " << count << '\n';
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
