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
        require(axtp::SchemaObjectCodec::validate("CastGetPinCodeConfigParams",nlohmann::json::object()),"empty legacy PIN selector remains valid");
        const nlohmann::json codeQuery={{"protocol","airplay"},{"includeSecret",true},{"includeSensitive",true},{"attemptId","18446744073709551615"}};
        require(axtp::SchemaObjectCodec::validate("CastGetPinCodeConfigParams",codeQuery),"explicit PIN protocol, sensitivity alias and exact attempt validate");
        const nlohmann::json challenge={{"attemptId","18446744073709551615"},{"challengeId","9007199254740993"},{"displayRequestId","2"},{"pinCode","1234"}};
        const auto challengeWire=axtp::SchemaObjectCodec::encodeTlv8("CastPinCodeChallenge",challenge);
        require(challengeWire&&axtp::SchemaObjectCodec::decodeTlv8("CastPinCodeChallenge",*challengeWire)==challenge,"credential challenge preserves exact uint64 strings through TLV");
        auto futureChallenge=challenge;futureChallenge["futureOptional"]=true;
        require(axtp::SchemaObjectCodec::decodeJson("CastPinCodeChallenge",futureChallenge.dump()).has_value(),"credential future optional JSON compatible");
        auto challengeExtra=*challengeWire;challengeExtra.insert(challengeExtra.end(),{0xFE,1,0x01});
        require(axtp::SchemaObjectCodec::decodeTlv8("CastPinCodeChallenge",challengeExtra)==challenge,"credential future optional TLV compatible");
        for(const auto& bad:nlohmann::json::array({1,"01","-1","18446744073709551616"})) {
            auto invalid=codeQuery;invalid["attemptId"]=bad;
            require(!axtp::SchemaObjectCodec::validate("CastGetPinCodeConfigParams",invalid),"credential selector rejects lossy or noncanonical identity");
        }
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
        const nlohmann::json focus={{"controlVersion",1},{"runtimeInstanceId","11111111-1111-4111-8111-111111111111"},
            {"clientInstanceId","22222222-2222-4222-8222-222222222222"},{"commandId","33333333-3333-4333-8333-333333333333"},
            {"target",{{"session",{{"id","source"},{"generation","18446744073709551615"}}},{"slotId",0},{"bindingGeneration","9007199254740993"},{"lifecycleGeneration","1"}}},
            {"expectedMeetingRevision","9007199254740995"},{"display","sidebar"},{"sidebarPage",1}};
        require(axtp::SchemaObjectCodec::decodeJson("CastSetPresentationFocusParams",focus.dump())==focus,"meeting full JSON preserves nested identity and uint64");
        require(!axtp::SchemaObjectCodec::encodeTlv8("CastSetPresentationFocusParams",focus),"unbound enum TLV must fail closed instead of guessing a wire mapping");
        const auto focusWire=axtp::SchemaObjectCodec::encodeTlv8("CastControlTarget",focus.at("target"));
        require(focusWire&&axtp::SchemaObjectCodec::decodeTlv8("CastControlTarget",*focusWire)==focus.at("target"),"nested target uint64 TLV roundtrip");
        auto futureFocus=focus;futureFocus["unknownOptional"]=true;
        require(axtp::SchemaObjectCodec::decodeJson("CastSetPresentationFocusParams",futureFocus.dump()).has_value(),"meeting future optional JSON compatible");
        for(const auto& bad:nlohmann::json::array({1,"01","-1","18446744073709551616"})){
            futureFocus=focus;futureFocus["expectedMeetingRevision"]=bad;
            require(!axtp::SchemaObjectCodec::validate("CastSetPresentationFocusParams",futureFocus),"meeting invalid decimal revision rejected");
        }
        auto expanded=focus;expanded.erase("sidebarPage");expanded["display"]="focus";
        require(axtp::SchemaObjectCodec::decodeJson("CastSetPresentationFocusParams",expanded.dump())==expanded,"adopted Focus display roundtrips exact identity through generated facts");
        expanded["display"]="not-adopted";require(!axtp::SchemaObjectCodec::validate("CastSetPresentationFocusParams",expanded),"unknown view cannot acquire semantics");
        const nlohmann::json patch={{"presentationAudioFollow",false}};
        const auto patchWire=axtp::SchemaObjectCodec::encodeTlv8("CastPresentationSettingsPatch",patch);
        require(patchWire&&axtp::SchemaObjectCodec::decodeTlv8("CastPresentationSettingsPatch",*patchWire)==patch,"false follow preference survives TLV encoding");
        for(const char* field:{"presentationFocusOnDoubleClick","presentationGridOnDoubleClick"})for(bool enabled:{false,true}){
            const nlohmann::json preference={{field,enabled}};
            const auto wire=axtp::SchemaObjectCodec::encodeTlv8("CastPresentationSettingsPatch",preference);
            require(wire&&axtp::SchemaObjectCodec::decodeTlv8("CastPresentationSettingsPatch",*wire)==preference,"Focus preference true/false survives generated TLV encoding");
            require(axtp::SchemaObjectCodec::decodeJson("CastPresentationSettingsPatch",preference.dump())==preference,"Focus preference survives JSON encoding");
        }
        require(axtp::MethodRegistry::fromGeneratedDefaults().findMethodId("cast.setPresentationFocus")==0x161B,"meeting ID comes from generated registry");
        const nlohmann::json attempt={{"attemptId","18446744073709551615"},{"challengeId","9007199254740993"},{"displayRequestId","0"},{"state","waitingForPassword"}};
        require(axtp::SchemaObjectCodec::decodeJson("CastAirPlayAuthenticationAttempt",attempt.dump())==attempt,"authentication identities preserve uint64 strings");
        for(const auto* field:{"attemptId","challengeId","displayRequestId"})for(const auto& invalid:nlohmann::json::array({1,"01","-1","18446744073709551616"})){
            auto bad=attempt;bad[field]=invalid;require(!axtp::SchemaObjectCodec::validate("CastAirPlayAuthenticationAttempt",bad),"authentication identity rejects malformed decimal");
        }
        require(axtp::SchemaObjectCodec::validate("CastPresentationSettingsPatch",{{"layoutMode","grid"}})&&axtp::SchemaObjectCodec::validate("CastPresentationSettingsPatch",{{"contentAdaptation","fixed"}}),"layout and adaptation have independent generated facts");
        require(!axtp::SchemaObjectCodec::validate("CastPresentationSettingsPatch",{{"layoutMode","fixed-ratio"}}),"unpublished old Layout values intentionally replaced");
        require(axtp::MethodRegistry::fromGeneratedDefaults().findMethodId("cast.stopSession")==0x1602&&axtp::MethodRegistry::fromGeneratedDefaults().findMethodId("cast.setMuted")==0x1607,"legacy routes preserve their method IDs");
        std::cout << "cast object codec vectors passed: " << count << '\n';
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
