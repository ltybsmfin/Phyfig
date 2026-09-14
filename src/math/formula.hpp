#pragma once
#include "imgui.h"
#include "formulas/formula_cache.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <string>

// Formula masks join the existing ImGui font atlas, so its Vulkan lifetime and
// resize handling cover text and mathematics. Runtime workers supply CPU masks;
// only the render thread rebuilds/uploads the atlas between frames.
namespace math_text {
inline std::vector<int> rectangles;
struct Mask { std::string name; int width,height; std::vector<unsigned char> rle; };
inline std::vector<Mask> custom;
inline std::vector<Mask> pendingCustom;
inline bool pending=false;
inline void setCustom(std::vector<Mask> masks) {
    bool same=masks.size()==custom.size();
    for(size_t i=0;same && i<masks.size();++i) same=masks[i].name==custom[i].name && masks[i].width==custom[i].width && masks[i].height==custom[i].height && masks[i].rle==custom[i].rle;
    if(!same){pendingCustom=std::move(masks);pending=true;}
    else {pendingCustom.clear();pending=false;}
}
inline size_t builtinCount(){return sizeof(formula_cache::entries)/sizeof(formula_cache::entries[0]);}
inline formula_cache::Entry entry(size_t index){
    if(index<builtinCount())return formula_cache::entries[index];
    const auto& a=custom.at(index-builtinCount());return {a.name.c_str(),a.width,a.height,a.rle.data(),a.rle.size()};
}
inline void initialize(ImFontAtlas& atlas) {
    if(pending)custom=std::move(pendingCustom);
    rectangles.clear();
    atlas.CustomRects.clear();
    atlas.PackIdMouseCursors=atlas.PackIdLines=-1;
    for(size_t i=0;i<builtinCount()+custom.size();++i){const auto formula=entry(i);
        rectangles.push_back(atlas.AddCustomRectRegular(formula.width, formula.height));
    }
    atlas.ClearTexData();
    if(!atlas.Build())throw std::runtime_error("Cannot build formula atlas");
    unsigned char* pixels = nullptr; int width=0, height=0;
    atlas.GetTexDataAsRGBA32(&pixels,&width,&height);
    if (!pixels) throw std::runtime_error("Cannot build formula atlas");
    for (std::size_t i=0;i<rectangles.size();++i) {
        const auto formula=entry(i);
        const auto* rect=atlas.GetCustomRectByIndex(rectangles[i]);
        if (!rect->IsPacked()) throw std::runtime_error("Formula did not fit in font atlas");
        int position=0;
        for (std::size_t p=0;p<formula.bytes;p+=2) {
            if (p+1>=formula.bytes) throw std::runtime_error("Invalid formula RLE");
            for(int n=0;n<formula.rle[p];++n) {
                if (position>=formula.width*formula.height) throw std::runtime_error("Formula mask overflow");
                const int x=rect->X+position%formula.width, y=rect->Y+position/formula.width;
                if (x>=width || y>=height) throw std::runtime_error("Formula atlas bounds");
                auto* destination=pixels+4*(y*width+x);
                destination[0]=destination[1]=destination[2]=255; destination[3]=formula.rle[p+1];
                ++position;
            }
        }
        if(position!=formula.width*formula.height) throw std::runtime_error("Incomplete formula mask");
    }
    pending=false;
}
inline int find(const char* name) {
    for (std::size_t i=0;i<rectangles.size();++i)
        if(std::strcmp(name,entry(i).name)==0) return static_cast<int>(i);
    throw std::runtime_error("Unknown formula name");
}
inline ImVec2 size(const char* name, float scale=1) {
    const auto f=entry(find(name));
    return {f.width*formula_cache::displayScale*scale,f.height*formula_cache::displayScale*scale};
}
inline void draw(ImDrawList* list, ImVec2 p, const char* name, ImU32 tint, float scale=1) {
    const int index=find(name);
    auto* atlas=ImGui::GetIO().Fonts;
    ImVec2 uv0,uv1;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(rectangles[index]),&uv0,&uv1);
    const ImVec2 extent=size(name,scale);
    list->AddImage(atlas->TexID,p,{p.x+extent.x,p.y+extent.y},uv0,uv1,tint);
}
inline void line(const char* name, ImU32 tint=IM_COL32(229,237,245,255)) {
    const ImVec2 natural=size(name);
    const float scale=natural.x>0?std::min(1.0f,ImGui::GetContentRegionAvail().x/natural.x):1;
    const ImVec2 extent=size(name,scale);
    draw(ImGui::GetWindowDrawList(),ImGui::GetCursorScreenPos(),name,tint,scale);
    ImGui::Dummy({extent.x,extent.y+3});
}
}
