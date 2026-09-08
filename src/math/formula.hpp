#pragma once
#include "imgui.h"
#include "formulas/formula_cache.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <vector>

// Formula masks join the existing ImGui font atlas, so its Vulkan lifetime and
// resize handling cover both text and mathematics. No runtime TeX subprocesses.
namespace math_text {
inline std::vector<int> rectangles;
inline void initialize(ImFontAtlas& atlas) {
    rectangles.clear();
    for (const auto& formula : formula_cache::entries)
        rectangles.push_back(atlas.AddCustomRectRegular(formula.width, formula.height));
    unsigned char* pixels = nullptr; int width=0, height=0;
    atlas.GetTexDataAsRGBA32(&pixels,&width,&height);
    if (!pixels) throw std::runtime_error("Cannot build formula atlas");
    for (std::size_t i=0;i<rectangles.size();++i) {
        const auto& formula=formula_cache::entries[i];
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
}
inline int find(const char* name) {
    for (std::size_t i=0;i<rectangles.size();++i)
        if(std::strcmp(name,formula_cache::entries[i].name)==0) return static_cast<int>(i);
    throw std::runtime_error("Unknown formula name");
}
inline ImVec2 size(const char* name, float scale=1) {
    const auto& f=formula_cache::entries[find(name)];
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
