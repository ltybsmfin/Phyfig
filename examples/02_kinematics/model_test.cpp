#include "model.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

void near(double a,double b,const char* message) {
    if (std::abs(a-b)>1e-8*std::max(1.0,std::abs(b))) throw std::runtime_error(message);
}
int main() {
    try {
        physics::Projectile standard;
        near(standard.range(),400.0/9.8,"45 degree level-ground range");
        near(standard.at(standard.apexTime()).vy,0,"Apex vertical velocity");
        near(standard.at(standard.flightTime()).y,0,"Landing height");
        near(standard.at(standard.flightTime()).vy,-standard.vy0,"Level-ground vertical symmetry");
        physics::Projectile horizontal({15,0,10,9.8});
        near(horizontal.flightTime(),std::sqrt(20.0/9.8),"Horizontal launch time");
        physics::Projectile vertical({15,90,0,9.8});
        near(vertical.range(),0,"Vertical launch range");
        physics::Projectile ground({15,0,0,9.8});
        near(ground.flightTime(),0,"Zero-duration ground launch");
        physics::Projectile raised({20,35,8,9.8});
        for (int i=0;i<=50;++i) {
            const auto s=raised.at(raised.flightTime()*i/50.0);
            near(s.vx,raised.vx0,"Horizontal velocity constant");
            near(0.5*(s.vx*s.vx+s.vy*s.vy)+9.8*s.y,0.5*400+9.8*8,"Energy consistency");
            const double h=0.00001;
            if(i>0 && i<50) {
                const auto next=raised.at(s.t+h),prev=raised.at(s.t-h);
                near((next.y-prev.y)/(2*h),s.vy,"Position derivative equals velocity");
            }
        }
        near(raised.at(-1).t,0,"Clamp before launch");
        near(raised.at(100).t,raised.flightTime(),"Clamp after landing");
        physics::Playback playback;
        playback.playing=true; playback.advance(100,2);
        if(playback.playing) throw std::runtime_error("Stop at end");
        near(playback.time,2,"End clamp");
        playback.seek(0.5,2); playback.advance(1,2); near(playback.time,0.5,"Scrubbing pauses");
        playback.rate=0.25; playback.playing=true; playback.advance(1,2); near(playback.time,0.75,"Slow playback");
        for (auto p : {physics::Parameters{0,45,0,9.8}, {20,45,-1,9.8}, {20,45,0,0},
            {20,91,0,9.8}, {std::numeric_limits<double>::quiet_NaN(),45,0,9.8}}) {
            bool rejected=false;try { physics::Projectile invalid(p); } catch(const std::invalid_argument&) { rejected=true; }
            if(!rejected) throw std::runtime_error("Invalid parameters accepted");
        }
        std::cout<<"Projectile solutions, conservation, derivative, limits and playback: passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
