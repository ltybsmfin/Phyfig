#pragma once
#include "model.hpp"
#include "imgui.h"
#include <array>

// A concrete lesson prototype, not a generic scene graph / solver framework.
class KinematicsLesson {
public:
    void draw(float dt);
    void selectStep(int step);
    void smokeCheck();
    void smokeScenario(int scenario);
    void captureFrame(unsigned frame,unsigned count);
    int step() const { return step_; }
    ImVec2 stepButtonCenter(int step) const { return stepCenters_.at(step); }
private:
    physics::Parameters parameters_{15,90,0,9.8};
    physics::Playback playback_{0,0.6,true};
    int step_ = 2;
    bool vectors_ = true, projections_ = true;
    bool sketch_ = true, graphs_ = false, keyPositions_ = true;
    std::array<ImVec2,4> stepCenters_{};
    void sidebar();
    void scene(ImVec2 size);
    void sketch(ImVec2 size);
    void graph(ImVec2 size, bool velocity);
    void transport();
};
