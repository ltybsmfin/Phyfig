#include "lesson.hpp"
#include "formula.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
constexpr ImU32 ink=IM_COL32(31,41,51,255), soft=IM_COL32(112,122,130,255);
constexpr ImU32 blue=IM_COL32(20,109,179,255), purple=IM_COL32(128,76,147,255);
constexpr ImU32 pencil=IM_COL32(182,187,189,255), paper=IM_COL32(250,249,245,255);
ImVec2 offset(ImVec2 p,float x,float y) { return {p.x+x,p.y+y}; }
std::string fixed(double value) { char b[64];std::snprintf(b,sizeof(b),"%.2f",value);return b; }
void label(ImDrawList* d,ImVec2 p,const std::string& s,ImU32 color=ink) { d->AddText(p,color,s.c_str()); }
void vectorArrow(ImDrawList* d,ImVec2 a,ImVec2 b,ImU32 color,float thickness=3) {
    const float dx=b.x-a.x,dy=b.y-a.y,n=std::hypot(dx,dy);
    if(n<2) return;
    const float ux=dx/n,uy=dy/n,h=std::min(11.0f,n*0.5f);
    d->AddLine(a,b,color,thickness);
    d->AddTriangleFilled(b,{b.x-h*ux+5*uy,b.y-h*uy-5*ux},{b.x-h*ux-5*uy,b.y-h*uy+5*ux},color);
}
void broken(ImDrawList* d,ImVec2 a,ImVec2 b,ImU32 color) {
    const float dx=b.x-a.x,dy=b.y-a.y,n=std::hypot(dx,dy);
    if(n<1) return;
    for(float t=0;t<n;t+=11) {
        const float end=std::min(n,t+5);
        d->AddLine({a.x+dx*t/n,a.y+dy*t/n},{a.x+dx*end/n,a.y+dy*end/n},color,1.5f);
    }
}
}

void KinematicsLesson::sketch(ImVec2 size) {
    const auto p=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("motion_sketch",size);
    auto* d=ImGui::GetWindowDrawList();
    d->AddRectFilled(p,offset(p,size.x,size.y),paper,10);
    d->PushClipRect(p,offset(p,size.x,size.y),true);
    const physics::Projectile model(parameters_);
    const auto state=model.at(playback_.time);
    const bool vertical=model.vx0<1e-7;
    label(d,offset(p,24,17),vertical?"竖直上抛 · 运动示意动画":"抛体运动 · 运动示意动画");
    label(d,offset(p,24,44),"实心球是当前物体；空心标记是关键位置。",soft);
    const float split=size.x*0.59f;
    d->AddLine(offset(p,split,76),offset(p,split,size.y-22),IM_COL32(219,218,211,255));
    const float floor=p.y+size.y-96;
    const float plotHeight=std::max(90.0f,size.y-224);
    const double meterScale=std::min((split-122)/std::max(1.0,model.range()*1.13),
                                     plotHeight/std::max(2.0,model.maxHeight()*1.15));
    const float x0=p.x+(vertical?split*0.40f:62.0f);
    auto point=[&](double x,double y) { return ImVec2(x0+static_cast<float>(x*meterScale),floor-static_cast<float>(y*meterScale)); };
    // A ground cross-section makes the physical scene readable without chart axes.
    d->AddLine({p.x+24,floor},{p.x+split-22,floor},ink,3);
    for(float x=p.x+38;x<p.x+split-25;x+=24) d->AddLine({x,floor+3},{x-13,floor+23},pencil,1.5f);
    label(d,{p.x+28,floor+36},"地面",soft);
    const auto start=point(0,parameters_.height),ball=point(state.x,state.y);
    if(parameters_.height>0) {
        d->AddRectFilled({x0-18,start.y},{x0-3,floor},IM_COL32(220,220,211,255));
        d->AddLine({x0-32,start.y},{x0+18,start.y},ink,2);
        math_text::draw(d,{x0-52,(start.y+floor)*0.5f},"height",ink);
    }
    const auto top=point(model.vx0*model.apexTime(),model.maxHeight());
    if(keyPositions_ && model.flightTime()>0) {
        if(vertical) broken(d,start,top,pencil);
        else {
            ImVec2 previous=start;
            for(int i=1;i<=80;++i) {
                const auto s=model.at(model.flightTime()*i/80.0);const auto next=point(s.x,s.y);
                if(i%2) d->AddLine(previous,next,pencil,1.5f); previous=next;
            }
        }
        d->AddCircle(start,5,ink,24,1.5f);
        d->AddCircle(top,6,soft,24,1.5f);
        label(d,offset(top,14,-26),"最高点",soft);
        math_text::draw(d,offset(top,14,-4),"apex",soft,0.85f);
    }
    if(step_>=2 && state.t>0) {
        // Recent physical positions show motion without manufacturing a second trajectory.
        for(int i=8;i>=1;--i) {
            const double time=state.t-model.flightTime()*0.016*i;
            if(time<0) continue;
            const auto past=model.at(time);
            const int alpha=20+(8-i)*13;
            d->AddCircleFilled(point(past.x,past.y),3.5f,IM_COL32(20,109,179,alpha));
        }
    }
    const double maximumSpeed=std::sqrt(parameters_.speed*parameters_.speed+2*parameters_.gravity*parameters_.height);
    const float speedScale=std::min(78.0f,plotHeight*0.27f)/static_cast<float>(maximumSpeed);
    if(keyPositions_ && step_>=1) {
        const ImVec2 tail=offset(start,vertical?85.0f:0.0f,0);
        const ImVec2 head=offset(tail,static_cast<float>(model.vx0)*speedScale,-static_cast<float>(model.vy0)*speedScale);
        vectorArrow(d,tail,head,soft,2);
        math_text::draw(d,offset(head,8,-19),"initial",soft);
    }
    if(vectors_) {
        // g only illustrates direction, and never shares the velocity scale.
        const float gx=vertical?x0-72:p.x+split-52;
        const float gy=p.y+116;
        vectorArrow(d,{gx,gy},{gx,gy+58},purple,2.5f);
        math_text::draw(d,{gx+13,gy+17},"gravity",purple);
        if(std::hypot(state.vx,state.vy)>0.03) {
            const auto tail=offset(ball,vertical?26.0f:0.0f,0);
            const auto head=offset(tail,static_cast<float>(state.vx)*speedScale,-static_cast<float>(state.vy)*speedScale);
            vectorArrow(d,tail,head,blue,3);
            const float labelX=vertical?(state.vy>=0?head.x-86:head.x+10):head.x+10;
            math_text::draw(d,{labelX,head.y-20},"velocity",blue);
        } else math_text::draw(d,offset(ball,25,21),"zero",blue);
    }
    d->AddCircleFilled(ball,13,IM_COL32(20,109,179,26));
    d->AddCircleFilled(ball,8,blue);
    d->AddCircleFilled(offset(ball,-2,-2),2,IM_COL32(220,241,251,255));
    const bool landed=model.flightTime()>0 && state.t>=model.flightTime()-1e-7;
    const char* phase=landed?"落地前瞬间：不模拟碰撞":
        (std::abs(state.vy)<0.03?(vertical?"最高点：瞬时速度为零，重力仍向下":"最高点：竖直速度为零，仍向前运动"):
        (state.vy>0?"上升：速度的竖直分量逐渐减小":"下落：向下的速度逐渐增大"));
    if(model.flightTime()==0) phase="没有空中运动区间：请增加高度或抛射角";
    label(d,{p.x+24,p.y+size.y-32},phase,blue);
    label(d,{p.x+split-150,p.y+83},"t = "+fixed(state.t)+" s",blue);

    // This is the user's interval/state sketch: a symbolic relation, not an x-axis.
    const float rx=p.x+split+24,rw=size.x-split-48;
    label(d,{rx,p.y+88},"初始状态  →  当前状态",ink);
    const float segmentY=p.y+174;
    const ImVec2 a{rx+20,segmentY},b{rx+rw-42,segmentY};
    d->AddLine(a,b,ink,2);
    d->AddCircleFilled(a,4,ink); d->AddCircleFilled(b,4,blue);
    math_text::draw(d,offset(a,-9,-43),"initial",ink);
    math_text::draw(d,offset(b,-15,-43),"velocity",blue);
    math_text::draw(d,offset(a,-12,18),"time_zero",ink,0.9f);
    math_text::draw(d,offset(b,-15,18),"time_now",blue,0.9f);
    math_text::draw(d,{rx+rw*0.5f-45,segmentY+58},"interval",ink,0.95f);
    math_text::draw(d,{rx+rw*0.5f-35,segmentY+98},"acceleration",purple);
    if(size.y>440) {
        const float fy=segmentY+157;
        d->AddLine({rx,fy-8},{rx+rw,fy-8},pencil);
        label(d,{rx,fy+6},"竖直速度： "+fixed(model.vy0)+" → "+fixed(state.vy)+" m/s",blue);
        label(d,{rx,fy+34},"当前高度： "+fixed(state.y)+" m",soft);
        if(step_>=2) {
            math_text::draw(d,{rx,fy+77},"delta_velocity",ink,0.9f);
            if(size.y>525) math_text::draw(d,{rx,fy+122},"delta_position",ink,0.9f);
        }
    }
    label(d,{rx,p.y+size.y-56},"横线表示状态先后关系",soft);
    label(d,{rx,p.y+size.y-30},"不是水平运动轨迹，也不是距离比例尺",soft);
    d->PopClipRect();
}
