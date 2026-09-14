#pragma once
#include "document.hpp"
#include <set>

namespace physics_editor::formula_resources {
inline void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline void validate(const Json& resources) {
    if(!resources.contains("formulaMasks"))return;
    require(resources.at("formulaMasks").is_array() && resources["formulaMasks"].size()<=192,"Formula library limit is 192 assets");
    require(resources.at("formulaScale").is_number() && resources["formulaScale"]>0 && resources["formulaScale"]<=4,"Invalid formula display scale");
    std::set<std::string> names;size_t pixels=0;
    for(const auto& a:resources["formulaMasks"]){
        const auto name=a.at("name").get<std::string>();
        require(!name.empty() && name.size()<=128 && names.insert(name).second,"Invalid or duplicate formula id");
        require(a.at("width").is_number_integer() && a.at("height").is_number_integer(),"Invalid formula dimensions");
        const int w=a["width"],h=a["height"];
        require(w>0 && w<=2048 && h>0 && h<=1024 && w*h<=1048576,"Formula exceeds image limits");
        pixels+=size_t(w)*h;require(pixels<=8*1048576,"Formula library exceeds pixel budget");
        const auto& r=a.at("rle");require(r.is_array() && r.size()%2==0 && r.size()<=size_t(w)*h*2,"Invalid formula RLE");
        size_t count=0;
        for(size_t i=0;i<r.size();i+=2){
            require(r[i].is_number_integer() && r[i+1].is_number_integer() && r[i]>0 && r[i]<=255 && r[i+1]>=0 && r[i+1]<=255,"Invalid formula RLE byte");
            count+=r[i].get<unsigned>();require(count<=size_t(w)*h,"Formula mask overflow");
        }
        require(count==size_t(w)*h,"Incomplete formula mask");
        if(a.contains("latex"))require(a["latex"].is_string() && a["latex"].get_ref<const std::string&>().size()<=4096,"Invalid formula source");
    }
}
inline void scene(const Json& doc,const Json& resources){
    if(!resources.contains("formulaMasks"))return;
    for(const auto& o:doc.at("objects"))if(o["type"]=="formula"){
        bool found=false;for(const auto& a:resources["formulaMasks"])if(a["name"]==o["properties"]["formula"])found=true;
        require(found,"Unknown formula asset; create a LaTeX formula or query get_formulas");
    }
    if(doc.contains("components"))for(const auto& d:doc["components"])scene(d,resources);
}
}
