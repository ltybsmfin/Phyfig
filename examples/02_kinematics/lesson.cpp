#include "lesson.hpp"
#include "formula.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
const ImU32 cyan = IM_COL32(75, 216, 204, 255);
const ImU32 amber = IM_COL32(248, 186, 95, 255);
const ImU32 white = IM_COL32(229, 237, 245, 255);
const ImU32 muted = IM_COL32(139, 158, 181, 255);
const ImU32 grid = IM_COL32(41, 54, 72, 255);
const ImU32 violet = IM_COL32(189, 159, 248, 255);
const ImU32 card = IM_COL32(18, 25, 36, 255);
ImVec2 add(ImVec2 a, ImVec2 b) { return {a.x+b.x, a.y+b.y}; }
std::string number(double value, int decimals = 2) {
    char buffer[80]; std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value); return buffer;
}
void text(ImDrawList* draw, ImVec2 p, ImU32 color, const std::string& value) { draw->AddText(p, color, value.c_str()); }
void dashed(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color) {
    const float dx = b.x-a.x, dy = b.y-a.y, length = std::hypot(dx,dy);
    if (length < 1) return;
    for (float t = 0; t < length; t += 10) {
        const float end = std::min(length, t+5);
        draw->AddLine({a.x+dx*t/length,a.y+dy*t/length}, {a.x+dx*end/length,a.y+dy*end/length}, color, 1.3f);
    }
}
void arrow(ImDrawList* draw, ImVec2 from, ImVec2 to, ImU32 color, float width = 2.5f) {
    const float dx = to.x-from.x, dy = to.y-from.y, length = std::hypot(dx,dy);
    if (length < 2) return;
    draw->AddLine(from, to, color, width);
    const float h = std::min(9.0f, length*0.4f), ux = dx/length, uy = dy/length;
    draw->AddTriangleFilled(to, {to.x-h*ux+4*uy,to.y-h*uy-4*ux}, {to.x-h*ux-4*uy,to.y-h*uy+4*ux}, color);
}
void paragraph(const char* value) { ImGui::TextWrapped("%s", value); }
void heading(const char* value) {
    ImGui::Spacing(); ImGui::TextColored(ImVec4(0.36f,0.85f,0.8f,1), "%s", value); ImGui::Separator();
}
const char* stepTitles[] = {"01  建立模型", "02  分解初速度", "03  合成运动", "04  求解与检验"};
const char* stepSubtitles[] = {
    "规定参考系、原点和已知条件",
    "把一个二维运动分解为两个方向",
    "用同一个时间连接轨迹与函数图像",
    "用落地条件求时间，再求水平射程"
};
}

void KinematicsLesson::selectStep(int step) {
    step_ = std::clamp(step, 0, 3);
    const physics::Projectile model(parameters_);
    playback_.seek(step_ < 2 ? 0 : model.apexTime(), model.flightTime());
}

void KinematicsLesson::sidebar() {
    ImGui::TextDisabled("解题路径 / 点击逐步展开");
    for (int i = 0; i < 4; ++i) {
        if (i == step_) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f,0.36f,0.37f,1));
        const bool clicked = ImGui::Button(stepTitles[i], {-1, 39});
        const auto p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
        stepCenters_[i] = {(p.x+q.x)*0.5f,(p.y+q.y)*0.5f};
        if (i == step_) ImGui::PopStyleColor();
        if (clicked) selectStep(i);
    }
    heading("已知条件");
    ImGui::SetNextItemWidth(-1);
    float speed = static_cast<float>(parameters_.speed);
    bool changed = ImGui::SliderFloat("##speed", &speed, 5, 35, "初速度 %.1f m/s", ImGuiSliderFlags_AlwaysClamp);
    parameters_.speed = speed;
    ImGui::SetNextItemWidth(-1);
    float angle = static_cast<float>(parameters_.angleDegrees);
    changed |= ImGui::SliderFloat("##angle", &angle, 0, 90, "抛射角 %.1f 度", ImGuiSliderFlags_AlwaysClamp);
    parameters_.angleDegrees = angle;
    ImGui::SetNextItemWidth(-1);
    float height = static_cast<float>(parameters_.height);
    changed |= ImGui::SliderFloat("##height", &height, 0, 15, "初始高度 %.1f m", ImGuiSliderFlags_AlwaysClamp);
    parameters_.height = height;
    math_text::line("gravity_value",muted);
    if (ImGui::Button("斜抛", {80,0})) { parameters_ = {}; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("平抛", {80,0})) { parameters_ = {15,0,10,9.8}; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("竖直上抛", {100,0})) { parameters_ = {15,90,0,9.8}; changed = true; }
    if (changed) selectStep(step_); // Rebuild from one physical model; never mix old time with new parameters.

    const physics::Projectile model(parameters_);
    const auto state = model.at(playback_.time);
    heading("本步推导");
    ImGui::PushTextWrapPos(0);
    if (step_ == 0) {
        paragraph("以地面为 y = 0，水平向右为 +x，竖直向上为 +y。小球从 (0, h0) 发射。");
        ImGui::Spacing();
        paragraph("先求什么？飞行时间 T 与落地点的水平射程 R。");
        ImGui::Spacing();
        paragraph("模型：质点、忽略空气阻力、重力加速度恒定、地面水平。");
    } else if (step_ == 1) {
        math_text::line("decompose_x",cyan);
        ImGui::Text("水平分量：%.2f m/s",model.vx0);
        math_text::line("decompose_y",amber);
        ImGui::Text("竖直分量：%.2f m/s",model.vy0);
        ImGui::Spacing();
        paragraph("青色：水平分量；橙色：竖直分量；白色：合速度。");
        paragraph("矢量分解不会产生两颗小球。两个方向共用同一个时间 t。");
    } else if (step_ == 2) {
        math_text::line("position_x",cyan);
        math_text::line("position_y",amber);
        math_text::line("velocity_x",cyan);
        math_text::line("velocity_y",amber);
        ImGui::Spacing();
        ImGui::Text("t = %.2f s", state.t);
        ImGui::Text("x = %.2f m    y = %.2f m", state.x, state.y);
        ImGui::Text("水平 %.2f / 竖直 %.2f m/s", state.vx, state.vy);
        paragraph("函数图像的横轴是时间；轨迹的横轴是位置 x。两者不能混淆。");
    } else {
        paragraph("落地条件：y(T) = 0，选择非负的落地时刻。");
        math_text::line("landing");
        math_text::line("flight",amber);
        ImGui::TextColored(ImVec4(0.98f,0.74f,0.4f,1), "T = %.3f s", model.flightTime());
        math_text::line("range",cyan);
        ImGui::Text("水平射程：%.3f m",model.range());
        math_text::line("peak");
        ImGui::Text("最高高度：%.2f m",model.maxHeight());
        ImGui::Spacing();
        paragraph("检查：落地点 y = 0；飞行中 vx 不变；最高点 vy = 0，但合速度未必为零。");
        if (parameters_.height > 0) paragraph("起落高度不同：不能套用 T = 2 vy0 / g。");
    }
    ImGui::PopTextWrapPos();
    if (parameters_.angleDegrees == 0 && parameters_.height == 0)
        ImGui::TextWrapped("边界情况：从地面水平发射，在此理想模型中 T = 0，不存在空中轨迹。");
}

void KinematicsLesson::scene(ImVec2 size) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("trajectory", size);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, add(origin,size), card, 10);
    draw->PushClipRect(origin, add(origin,size), true);
    text(draw, add(origin,{18,12}), white, "运动示意图  ·  x-y 轨迹");
    text(draw, add(origin,{18,37}), muted, "坐标等比例 / 长度单位 m / 速度箭头独立缩放");
    const physics::Projectile model(parameters_);
    const auto state = model.at(playback_.time);
    // Reserve space below y=0 for downward velocity vectors at landing.
    const float plotW = size.x-112, plotH = size.y-161;
    const double worldW = std::max(10.0, model.range()*1.20);
    const double worldH = std::max(6.0, model.maxHeight()*1.42);
    const float scale = static_cast<float>(std::min(plotW/worldW, plotH/worldH));
    const ImVec2 zero{origin.x+60,origin.y+size.y-88};
    auto map = [&](double x, double y) { return ImVec2(zero.x+static_cast<float>(x)*scale,zero.y-static_cast<float>(y)*scale); };
    const double maxX = plotW/scale, maxY = plotH/scale;
    double tick = 1;
    const double rough = std::max(maxX/9, maxY/5);
    const double magnitude = std::pow(10.0, std::floor(std::log10(std::max(0.01,rough))));
    for (double factor : {1.0,2.0,5.0,10.0}) if (factor*magnitude >= rough) { tick = factor*magnitude; break; }
    for (double x = tick; x < maxX; x += tick) {
        draw->AddLine(map(x,0), map(x,maxY), grid);
        text(draw, add(map(x,0),{-10,7}), muted, number(x,0));
    }
    for (double y = tick; y < maxY; y += tick) {
        draw->AddLine(map(0,y), map(maxX,y), grid);
        text(draw, add(map(0,y),{-38,-10}), muted, number(y,0));
    }
    arrow(draw, zero, map(maxX+0.4,0), muted, 1.5f);
    arrow(draw, zero, map(0,maxY+0.3), muted, 1.5f);
    text(draw, add(zero,{-18,6}), muted, "0");
    text(draw, add(map(maxX,0),{5,5}), muted, "x");
    text(draw, add(map(0,maxY),{-24,-14}), muted, "y");
    text(draw, add(zero,{0,31}), muted, "地面 y = 0");
    const auto launch = map(0, parameters_.height);
    draw->AddCircleFilled(launch, 4, white);
    if (parameters_.height > 0) {
        draw->AddRectFilled(map(-0.5,parameters_.height), map(-0.1,0), IM_COL32(53,66,84,255));
        text(draw, add(launch,{10,36}), muted, "h0 = " + number(parameters_.height,1) + " m");
    }
    if (step_>=2 && model.flightTime()<=0) {
        draw->AddCircleFilled(launch,6,white);
        text(draw,add(origin,{150,100}),amber,"T = 0：没有空中运动区间");
        text(draw,add(origin,{150,130}),muted,"增加发射高度或抛射角，再观察轨迹。");
        draw->PopClipRect(); return;
    }
    if (step_ >= 2) {
        ImVec2 previous = launch;
        for (int i = 1; i <= 180; ++i) {
            const auto sample = model.at(model.flightTime()*i/180.0);
            const auto point = map(sample.x,sample.y);
            draw->AddLine(previous, point, IM_COL32(60,100,107,255), 2);
            if (sample.t <= state.t) draw->AddLine(previous, point, cyan, 2.8f);
            previous = point;
        }
        for (int i = 0; i <= 8; ++i) {
            const auto sample = model.at(model.flightTime()*i/8.0);
            draw->AddCircleFilled(map(sample.x,sample.y), 2.5f, muted);
        }
        const auto apex = map(model.vx0*model.apexTime(), model.maxHeight());
        if (step_ == 3) {
            dashed(draw, apex, map(model.vx0*model.apexTime(),0), violet);
            text(draw, add(apex,{8,-27}), violet, "最高点  vy = 0");
            draw->AddCircle(apex, 6, violet, 24, 1.5f);
            const auto landing = map(model.range(),0);
            draw->AddCircle(landing, 6, amber, 24, 2);
            text(draw, add(landing,{-48,-30}), amber, "R = " + number(model.range()) + " m");
        }
        const auto ball = map(state.x,state.y);
        if (projections_) {
            dashed(draw, ball, map(state.x,0), cyan);
            dashed(draw, ball, map(0,state.y), amber);
            draw->AddCircleFilled(map(state.x,0), 4, cyan);
            draw->AddCircleFilled(map(0,state.y), 4, amber);
        }
        if (vectors_) {
            // One fixed speed scale for the whole flight, including the faster impact speed from h0 > 0.
            const double maxSpeed = std::sqrt(parameters_.speed*parameters_.speed+2*parameters_.gravity*parameters_.height);
            const float velocityScale = std::min(65.0f,std::min(plotW,plotH)*0.23f)/static_cast<float>(maxSpeed);
            const ImVec2 horizontal{ball.x+static_cast<float>(state.vx)*velocityScale,ball.y};
            const ImVec2 vertical{ball.x,ball.y-static_cast<float>(state.vy)*velocityScale};
            const ImVec2 sum{horizontal.x,vertical.y};
            arrow(draw,ball,horizontal,cyan); arrow(draw,ball,vertical,amber); arrow(draw,ball,sum,white,1.8f);
            text(draw,std::abs(state.vx)>0.2 ? add(horizontal,{4,4}) : add(ball,{8,35}),cyan,
                std::abs(state.vx)>0.2 ? "vx" : "vx = 0");
            if (std::abs(state.vy)>0.2) text(draw,add(vertical,{5,-18}),amber,"vy");
            else text(draw,add(ball,{8,12}),amber,"vy = 0");
            arrow(draw,add(ball,{-24,0}),add(ball,{-24,38}),violet,1.8f);
            text(draw,add(ball,{-48,39}),violet,"g");
        }
        draw->AddCircleFilled(ball, 10, IM_COL32(75,216,204,35));
        draw->AddCircleFilled(ball, 6, white);
        text(draw, {origin.x+size.x-208,origin.y+14},cyan,"t = " + number(state.t) + " s");
        text(draw, {origin.x+size.x-228,origin.y+37},muted,
            model.flightTime()>0 && state.t>=model.flightTime()-1e-8 ? "落地前瞬间 / 不模拟碰撞" : "等时间间隔采样点");
    } else {
        float vectorScale = std::min(plotW,plotH)*0.65f/static_cast<float>(parameters_.speed);
        if (model.vy0>0) vectorScale=std::min(vectorScale,(launch.y-origin.y-97)/static_cast<float>(model.vy0));
        if (model.vx0>0) vectorScale=std::min(vectorScale,(plotW-145)/static_cast<float>(model.vx0));
        vectorScale=std::max(0.1f,vectorScale);
        const ImVec2 horizontal{launch.x+static_cast<float>(model.vx0)*vectorScale,launch.y};
        const ImVec2 sum{horizontal.x,launch.y-static_cast<float>(model.vy0)*vectorScale};
        arrow(draw,launch,sum,white,3);
        text(draw,add(sum,{8,-14}),white,"v0");
        const ImVec2 legend{origin.x+size.x-230,origin.y+80};
        text(draw,legend,white,"v0 = " + number(parameters_.speed,1) + " m/s");
        if (step_ == 1) {
            arrow(draw,launch,horizontal,cyan);
            arrow(draw,horizontal,sum,amber);
            dashed(draw,sum,{launch.x,sum.y},amber);
            if (model.vx0>0.01) text(draw,add(horizontal,{8,8}),cyan,"vx0");
            if (model.vy0>0.01) text(draw,add(sum,{10,12}),amber,"vy0");
            text(draw,add(legend,{0,29}),cyan,"vx0 = " + number(model.vx0) + " m/s");
            text(draw,add(legend,{0,58}),amber,"vy0 = " + number(model.vy0) + " m/s");
        }
        if (parameters_.angleDegrees > 0) {
            draw->PathArcTo(launch, 36, -static_cast<float>(parameters_.angleDegrees*physics::pi/180), 0, 24);
            draw->PathStroke(amber, 0, 1.5f);
        }
        text(draw,add(launch,{42,-22}),amber,"a = " + number(parameters_.angleDegrees,0) + " deg");
        draw->AddCircleFilled(launch,6,white);
        text(draw, {origin.x+size.x-200,origin.y+14},cyan,"t = 0.00 s");
    }
    draw->PopClipRect();
}

void KinematicsLesson::graph(ImVec2 size, bool velocity) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(velocity ? "velocity_graph" : "position_graph",size);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,add(origin,size),card,10);
    draw->PushClipRect(origin,add(origin,size),true);
    text(draw,add(origin,{15,10}),white,velocity ? "速度 - 时间  /  v-t" : "位置 - 时间  /  x-t, y-t");
    if (step_ < 2) {
        text(draw,add(origin,{20,65}),muted,"第 3 步显示函数图像");
        text(draw,add(origin,{20,94}),muted,"横轴将变为时间 t，不是位置 x");
        draw->PopClipRect(); return;
    }
    const physics::Projectile model(parameters_);
    const auto state = model.at(playback_.time);
    if (model.flightTime()<=0) {
        text(draw,add(origin,{20,66}),amber,"T = 0，时间区间退化为一个点");
        text(draw,add(origin,{20,95}),muted,"此时不绘制函数曲线");
        draw->PopClipRect(); return;
    }
    const double end = std::max(model.flightTime(),0.01);
    const double lo = velocity ? -std::max(1.0, std::abs(model.at(end).vy))*1.18 : 0;
    const double hi = velocity ? std::max({1.0,model.vx0,model.vy0})*1.18 : std::max({1.0,model.range(),model.maxHeight()})*1.18;
    const float left=origin.x+51, right=origin.x+size.x-25, top=origin.y+56, bottom=origin.y+size.y-33;
    auto map = [&](double t, double value) {
        return ImVec2(left+static_cast<float>(t/end)*(right-left),bottom-static_cast<float>((value-lo)/(hi-lo))*(bottom-top));
    };
    for (int i=0;i<=4;++i) {
        const double value=lo+(hi-lo)*i/4;
        draw->AddLine(map(0,value),map(end,value),grid);
        text(draw,{origin.x+5,map(0,value).y-9},muted,number(value,0));
    }
    for (int i=0;i<=4;++i) {
        const double t=end*i/4;
        draw->AddLine(map(t,lo),map(t,hi),grid);
        text(draw,{map(t,lo).x-12,bottom+6},muted,number(t,1));
    }
    draw->AddLine(map(0,0),map(end,0),muted);
    text(draw,{right-30,origin.y+10},muted,"t / s");
    text(draw,{left,top-22},cyan,velocity ? "vx (m/s)" : "x (m)");
    text(draw,{left+110,top-22},amber,velocity ? "vy (m/s)" : "y (m)");
    ImVec2 prevX{},prevY{};
    for (int i=0;i<=120;++i) {
        const double t=model.flightTime()*i/120;
        const auto sample=model.at(t);
        auto x=map(t,velocity?sample.vx:sample.x), y=map(t,velocity?sample.vy:sample.y);
        if (i) { draw->AddLine(prevX,x,cyan,2); draw->AddLine(prevY,y,amber,2); }
        prevX=x;prevY=y;
    }
    dashed(draw,map(state.t,lo),map(state.t,hi),white);
    draw->AddCircleFilled(map(state.t,velocity?state.vx:state.x),4,cyan);
    draw->AddCircleFilled(map(state.t,velocity?state.vy:state.y),4,amber);
    draw->PopClipRect();
    if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        playback_.seek((ImGui::GetIO().MousePos.x-left)/(right-left)*model.flightTime(),model.flightTime());
    if (hovered) ImGui::SetTooltip("点击或拖动图像，定位同一时刻并暂停");
}

void KinematicsLesson::transport() {
    const physics::Projectile model(parameters_);
    const double end=model.flightTime();
    if (step_ < 2 || end<=0) ImGui::BeginDisabled();
    if (ImGui::Button(playback_.playing ? "暂停" : "播放",{72,0})) {
        if (!playback_.playing && playback_.time >= end) playback_.time=0;
        playback_.playing=!playback_.playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("重置时间")) playback_.seek(0,end);
    ImGui::SameLine();
    if (ImGui::Button("-0.05s")) playback_.seek(playback_.time-0.05,end);
    ImGui::SameLine();
    if (ImGui::Button("+0.05s")) playback_.seek(playback_.time+0.05,end);
    ImGui::SameLine();
    if (ImGui::Button("最高点")) playback_.seek(model.apexTime(),end);
    ImGui::SameLine();
    if (ImGui::Button("落地")) playback_.seek(end,end);
    ImGui::SameLine();
    bool slow=playback_.rate<0.6;
    if (ImGui::Checkbox("慢放",&slow)) playback_.rate=slow?0.25:0.6;
    float time=static_cast<float>(playback_.time);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time",&time,0,static_cast<float>(end),"t = %.3f s",ImGuiSliderFlags_AlwaysClamp))
        playback_.seek(time,end);
    if (step_ < 2 || end<=0) ImGui::EndDisabled();
    if (step_ >= 2) {
        ImGui::Checkbox("速度与加速度",&vectors_); ImGui::SameLine();
        if(!sketch_) { ImGui::Checkbox("位置投影线",&projections_); ImGui::SameLine(); }
        ImGui::TextDisabled("拖动时间轴定位；开启函数图像可联动观察");
    } else ImGui::TextDisabled("下一步揭示更多信息；第 3 步解锁播放和时间轴。");
}

void KinematicsLesson::draw(float dt) {
    const physics::Projectile model(parameters_);
    if (step_>=2) playback_.advance(dt,model.flightTime());
    auto& io=ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) selectStep(0);
        if (ImGui::IsKeyPressed(ImGuiKey_2)) selectStep(1);
        if (ImGui::IsKeyPressed(ImGuiKey_3)) selectStep(2);
        if (ImGui::IsKeyPressed(ImGuiKey_4)) selectStep(3);
        if (step_>=2 && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            if (!playback_.playing && playback_.time>=model.flightTime()) playback_.time=0;
            playback_.playing=!playback_.playing;
        }
    }
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Kinematics",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted("运动学实验室");
    ImGui::SetWindowFontScale(1);
    ImGui::SameLine(235); ImGui::TextDisabled("动态运动示意 / 从草稿到过程");
    ImGui::TextDisabled("观察物体怎么运动，再用状态关系与公式解释原因。示意箭头的长度不代表位移。");
    ImGui::Separator();
    const float sidebarWidth=320;
    const auto available=ImGui::GetContentRegionAvail();
    ImGui::BeginChild("sidebar",{sidebarWidth,available.y},ImGuiChildFlags_Borders);
    sidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("workspace",{0,available.y},ImGuiChildFlags_None);
    ImGui::Checkbox("草稿示意",&sketch_); ImGui::SameLine();
    ImGui::Checkbox("函数图像",&graphs_); ImGui::SameLine();
    ImGui::Checkbox("关键位置",&keyPositions_);
    ImGui::TextColored(ImVec4(0.37f,0.85f,0.81f,1),"%s",stepTitles[step_]);
    ImGui::SameLine(); ImGui::TextDisabled("%s",stepSubtitles[step_]);
    const float width=ImGui::GetContentRegionAvail().x;
    const float space=ImGui::GetContentRegionAvail().y;
    const float graphHeight=graphs_?std::clamp(space*0.27f,178.0f,240.0f):0;
    const float sceneHeight=std::max(sketch_?365.0f:270.0f,space-graphHeight-183);
    if(sketch_) sketch({width,sceneHeight}); else scene({width,sceneHeight});
    if(graphs_) {
        graph({(width-10)/2,graphHeight},false); ImGui::SameLine();
        graph({(width-10)/2,graphHeight},true);
    }
    transport();
    ImGui::Separator();
    if (step_>0) { if (ImGui::Button("上一步")) selectStep(step_-1); ImGui::SameLine(); }
    if (step_<3) { if (ImGui::Button("下一步：展开推导")) selectStep(step_+1); }
    else ImGui::TextDisabled("同一个时刻，对应同一组位置、速度与公式数值。");
    ImGui::EndChild();
    ImGui::End();
}

void KinematicsLesson::smokeCheck() {
    if (step_!=3) throw std::runtime_error("Step navigation did not reach the solution");
    const physics::Projectile model(parameters_);
    playback_.seek(model.apexTime(),model.flightTime());
    if (std::abs(model.at(playback_.time).vy)>1e-8) throw std::runtime_error("Apex marker mismatch");
    playback_.playing=true; playback_.advance(model.flightTime()*2,model.flightTime());
    if (playback_.playing || playback_.time!=model.flightTime()) throw std::runtime_error("Playback did not stop at landing");
    selectStep(0); if (playback_.time!=0 || playback_.playing) throw std::runtime_error("Step reset failed");
    selectStep(3);
    const physics::Projectile vertical({15,90,0,9.8});
    if(vertical.at(vertical.flightTime()*0.25).vy<=0 || vertical.at(vertical.flightTime()*0.75).vy>=0)
        throw std::runtime_error("Sketch velocity must reverse between ascent and descent");
    std::printf("Lesson button navigation, apex, landing and rewind: passed\n");
}

void KinematicsLesson::captureFrame(unsigned frame,unsigned count) {
    parameters_={15,90,0,9.8};
    step_=2; sketch_=true; graphs_=false;
    const physics::Projectile model(parameters_);
    playback_.seek(model.flightTime()*frame/std::max(1u,count-1),model.flightTime());
}

void KinematicsLesson::smokeScenario(int scenario) {
    sketch_=scenario!=2;
    graphs_=scenario==1 || scenario==2;
    if (scenario==0) parameters_={15,0,10,9.8};
    if (scenario==1) parameters_={15,90,0,9.8};
    if (scenario==2) parameters_={15,0,0,9.8};
    if (scenario==3) parameters_={5,90,15,9.8};
    if (scenario==4) parameters_={};
    selectStep(scenario==3?1:3);
    if (scenario==0) {
        const physics::Projectile model(parameters_);
        playback_.seek(model.flightTime(),model.flightTime());
    }
}
