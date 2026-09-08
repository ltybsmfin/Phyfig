#include "animation.hpp"
#include "expression.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace physics_editor;
namespace {
void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
void near(double a,double b){check(std::abs(a-b)<1e-8,"Unexpected animation value");}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid animation accepted");}
}
int main(){try{
    near(Expression("-2^2 + 2^3^2",{}).evaluate(Json::object()),508);
    near(Expression("sin(pi/2)+cos(0)+sind(30)",{}).evaluate(Json::object()),2.5);
    near(Expression("clamp(pow(t,2),0,9)",{"t"}).evaluate({{"t",4}}),9);
    rejects([]{Expression("system(1)",{});});rejects([]{Expression("t.__class__",{"t"});});
    rejects([]{Expression("sqrt(-1)",{}).evaluate(Json::object());});
    Document d({},animation::demo());
    near(d.evaluate("block",2).x,2);near(d.evaluate("block",3).x,4.5);near(d.evaluate("block",4).x,6.5);
    near(d.evaluate("block",std::nextafter(3.0,0.0)).y,0);near(d.evaluate("block",3).y,1.5);
    near(animation::properties(d.object("force"),.5,6)["dy"],2*std::sin(1));
    const auto initial=d.snapshot();
    d.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","set_keyframe"},{"id","block"},{"channel","y"},{"time",3},{"value",2.5},{"interpolation","smooth"}}})}}}});
    near(d.evaluate("block",3).y,2.5);near(d.evaluate("block",4.5).y,2);
    d.dispatch({{"method","undo"}});check(d.scene()==initial["document"],"Animation undo failed");
    auto bad=d.scene();bad["objects"][1]["animation"]["channels"]["x"]["segments"][1]["start"]=3.1;
    rejects([&]{d.dispatch({{"method","replace"},{"args",{{"document",bad}}}});});
    const auto before=d.snapshot();
    rejects([&]{d.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","set_keyframe"},{"id","block"},{"channel","y"},{"time",9},{"value",2}}})}}}});});
    check(before==d.snapshot(),"Bad keyframe partially committed");
    // Position animation does not silently invent a physical velocity at a jump.
    rejects([&]{d.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","create"},{"id","velocity"},{"type","arrow"},{"binding",{{"target","block"},{"kind","velocity"},{"scale",1}}}}})}}}});});
    std::ofstream("animation-demo-test.json")<<animation::demo().dump(2);
    std::cout<<"PASS: safe expressions, nonlinear formulas, right-continuous jumps, independent quantities, key interpolation, undo and atomic validation\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
