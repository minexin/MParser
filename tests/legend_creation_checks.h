#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

inline void checkLegendCreation(const nlohmann::json& graph, const nlohmann::json& expected) {
    using Json = nlohmann::json;
    const auto name = expected.at("name").get<std::string>();
    const auto require = [&](bool condition,const std::string& field) {
        if (!condition) { throw std::runtime_error(name+": "+field); }
    };
    Json labels=Json::array(),members=Json::array(),names={"<deleted>","<deleted>","<deleted>"};
    size_t legends=0;
    const auto memberIndex = [](const Json& line) {
        const auto& data=line.at("properties").at("YData");
        if (data.size()==2 && data[0].is_number() && data[1].is_number()) {
            for (int index=1;index<=3;++index) if (data[0]==index && data[1]==index+1) { return index; }
        }
        return 4;
    };
    const auto& objects=graph.at("objects");
    for (const auto& object:objects) {
        if (object.at("type")=="Line") {
            const auto index=memberIndex(object);
            if (index<=3) { names[index-1]=object.at("properties").at("DisplayName"); }
        }
        if (object.at("type")!="Legend") { continue; }
        ++legends;
        for (const auto& entry:object.at("entries")) {
            labels.push_back(entry.at("label"));
            const auto line=std::find_if(objects.begin(),objects.end(),[&](const auto& item) {return item.at("id")==entry.at("line");});
            require(line!=objects.end() && line->at("type")=="Line","dangling Legend Line");
            members.push_back(memberIndex(*line));
        }
        for (const auto& [property,field]:{std::pair{"Location","location"},std::pair{"Orientation","orientation"},
            std::pair{"AutoUpdate","auto"},std::pair{"Visible","visible"}}) {
            require(object.at("properties").at(property)==expected.at(field),std::string(property));
        }
        const auto axes=std::find_if(objects.begin(),objects.end(),[&](const auto& item) {return item.at("id")==object.at("axes");});
        require(axes!=objects.end() && axes->at("type")=="Axes","Legend Axes reference");
        const auto original=std::find_if(objects.begin(),objects.end(),[](const auto& item) {return item.at("type")=="Axes";});
        require((axes==original)==expected.at("associated_is_original").get<bool>(),"associated Axes");
    }
    auto expectedMembers=expected.at("members");
    if (expectedMembers.is_number()) { expectedMembers=Json::array({expectedMembers}); }
    require(legends==expected.at("legend_count").get<size_t>(),"Legend count");
    require(labels==expected.at("labels"),"labels "+labels.dump());
    require(members==expectedMembers,"members "+members.dump());
    require(names==expected.at("names"),"DisplayName "+names.dump());
}
