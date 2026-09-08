#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace physics {
constexpr double pi = 3.14159265358979323846;
struct Parameters {
    double speed = 20.0;       // m/s
    double angleDegrees = 45;  // degrees above +x
    double height = 0;         // m above the landing plane
    double gravity = 9.8;      // positive magnitude, m/s^2
};
struct State { double t, x, y, vx, vy; };
class Projectile {
public:
    explicit Projectile(Parameters p = {}) : parameters(p) {
        if (!std::isfinite(p.speed) || !std::isfinite(p.angleDegrees) || !std::isfinite(p.height)
            || !std::isfinite(p.gravity) || p.speed <= 0 || p.height < 0 || p.gravity <= 0
            || p.angleDegrees < 0 || p.angleDegrees > 90)
            throw std::invalid_argument("Require finite v0 > 0, h0 >= 0, g > 0, angle in [0,90]");
        const double radians = p.angleDegrees * pi / 180.0;
        vx0 = p.speed * std::cos(radians);
        if (p.angleDegrees == 90) vx0 = 0; // Exact vertical limit.
        vy0 = p.speed * std::sin(radians);
    }
    double flightTime() const {
        return (vy0 + std::sqrt(vy0 * vy0 + 2 * parameters.gravity * parameters.height)) / parameters.gravity;
    }
    double apexTime() const { return vy0 / parameters.gravity; }
    double range() const { return vx0 * flightTime(); }
    double maxHeight() const { return parameters.height + vy0 * vy0 / (2 * parameters.gravity); }
    State at(double t) const {
        if (!std::isfinite(t)) throw std::invalid_argument("Time must be finite");
        t = std::clamp(t, 0.0, flightTime());
        return {t, vx0 * t, std::max(0.0, parameters.height + vy0 * t - 0.5 * parameters.gravity * t * t),
            vx0, vy0 - parameters.gravity * t};
    }
    Parameters parameters;
    double vx0{}, vy0{};
};
// Playback is separate from physical time evaluation. Scrubbing is exact and pauses playback.
struct Playback {
    double time = 0;
    double rate = 1;
    bool playing = false;
    void seek(double value, double end) { time = std::clamp(value, 0.0, end); playing = false; }
    void advance(double dt, double end) {
        if (!playing) return;
        time = std::min(end, time + std::max(0.0, dt) * rate);
        if (time >= end) playing = false;
    }
};
}
