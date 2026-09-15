#include "component.hpp"
#include "document.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace physics_editor::components {
namespace {
constexpr double pi=3.14159265358979323846;
void require(bool ok,const std::string& error) { if(!ok) throw std::runtime_error(error); }
bool identifier(const std::string& name) {
    if(name.empty() || name.size()>64 || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0]=='_')) return false;
    for(unsigned char c:name) if(!(std::isalnum(c) || c=='_') || c>=128) return false;
    return true;
}
double finite(const Json& value) { require(value.is_number(),"Parameter requires a number"); const double v=value; require(std::isfinite(v) && std::abs(v)<=1e6,"Parameter/result outside finite +/- 1000000"); return v; }
class Parser {
    const std::string& s_; const Json& vars_; size_t pos_=0; unsigned depth_=0;
    void space() { while(pos_<s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    bool take(char c) { space(); if(pos_<s_.size() && s_[pos_]==c) { ++pos_; return true; } return false; }
    double primary() {
        require(++depth_<=32,"Expression nesting exceeds 32"); double value=0;
        if(take('+')) value=primary();
        else if(take('-')) value=-primary();
        else if(take('(')) { value=sum(); require(take(')'),"Missing closing parenthesis"); }
        else {
            space(); require(pos_<s_.size(),"Unexpected end of expression");
            if(std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_]=='.') {
                char* end=nullptr; value=std::strtod(s_.c_str()+pos_,&end); require(end!=s_.c_str()+pos_,"Invalid number"); pos_=static_cast<size_t>(end-s_.c_str());
            } else {
                const size_t begin=pos_;
                while(pos_<s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_]=='_')) ++pos_;
                const auto name=s_.substr(begin,pos_-begin); require(identifier(name),"Invalid expression identifier");
                if(take('(')) {
                    const double arg=sum(); require(take(')'),"Missing function parenthesis");
                    if(name=="sin") value=std::sin(arg*pi/180);
                    else if(name=="cos") value=std::cos(arg*pi/180);
                    else if(name=="sqrt") { require(arg>=0,"sqrt requires nonnegative input"); value=std::sqrt(arg); }
                    else if(name=="abs") value=std::abs(arg);
                    else throw std::runtime_error("Unknown function: "+name);
                } else { require(vars_.contains(name),"Unknown parameter: "+name); value=finite(vars_.at(name)); }
            }
        }
        --depth_; require(std::isfinite(value),"Non-finite expression result"); return value;
    }
    double product() { double v=primary(); for(;;) { if(take('*')) v*=primary(); else if(take('/')) { const double divisor=primary(); require(std::abs(divisor)>1e-12,"Division by zero"); v/=divisor; } else return v; } }
    double sum() { double v=product(); for(;;) { if(take('+')) v+=product(); else if(take('-')) v-=product(); else return v; } }
public:
    Parser(const std::string& s,const Json& vars):s_(s),vars_(vars) {}
    double run() { require(!s_.empty() && s_.size()<=512,"Expression size must be 1..512 bytes"); const double v=sum(); space(); require(pos_==s_.size(),"Unexpected expression suffix"); require(std::isfinite(v) && std::abs(v)<=1e6,"Expression result outside range"); return v; }
};
std::string format(const std::string& text,const Json& values) {
    require(text.size()<=4096,"Text template too long"); std::string result;
    for(size_t i=0;i<text.size();) {
        if(text[i]!='{') { result+=text[i++]; continue; }
        const auto end=text.find('}',i); require(end!=std::string::npos,"Unclosed text placeholder");
        const auto key=text.substr(i+1,end-i-1); require(values.contains(key),"Unknown text parameter: "+key);
        std::ostringstream number; number<<std::setprecision(5)<<values.at(key).get<double>(); result+=number.str(); i=end+1;
    }
    return result;
}
}
double expression(const std::string& source,const Json& variables) { return Parser(source,variables).run(); }
Json materialize(const Json& definition,const Json& overrides) {
    const auto& meta=definition.at("componentDefinition");
    const auto& parameters=meta.at("parameters");
    require(parameters.is_object() && parameters.size()<=32,"Component allows at most 32 named parameters");
    require(overrides.is_object(),"Parameter overrides must be an object");
    Json values=Json::object();
    for(auto it=parameters.begin();it!=parameters.end();++it) {
        require(identifier(it.key()),"Parameter names must be ASCII identifiers"); const auto& spec=it.value();
        require(!spec.contains("unit") || (spec["unit"].is_string() && spec["unit"].get_ref<const std::string&>().size()<=64),"Parameter unit must be a string of at most 64 bytes");
        const double low=finite(spec.at("min")),high=finite(spec.at("max")),fallback=finite(spec.at("default"));
        require(low<=fallback && fallback<=high,"Default parameter outside its range: "+it.key());
        const double value=overrides.contains(it.key())?finite(overrides.at(it.key())):fallback;
        require(low<=value && value<=high,"Parameter outside declared range: "+it.key()); values[it.key()]=value;
    }
    for(auto it=overrides.begin();it!=overrides.end();++it) require(parameters.contains(it.key()),"Unknown public parameter: "+it.key());
    const auto constraints=meta.value("constraints",Json::array()); require(constraints.is_array() && constraints.size()<=32,"Too many component constraints");
    for(const auto& c:constraints) require(expression(c.at("expression"),values)>0,c.value("message","Component constraint failed"));
    Json objects=definition.at("objects");
    const auto& bindings=meta.at("bindings"); require(bindings.is_array() && bindings.size()<=512,"Too many component bindings"); std::set<std::string> destinations;
    for(const auto& binding:bindings) {
        const std::string id=binding.at("object"),property=binding.at("property");
        require(destinations.insert(id+"/"+property).second,"Two bindings write the same property");
        auto it=std::find_if(objects.begin(),objects.end(),[&](const Json& o){return o.at("id")==id;});
        require(it!=objects.end(),"Binding target missing: "+id); auto& props=it->at("properties");
        require(props.contains(property),"Binding property missing: "+property);
        if(binding.contains("template")) { require(property=="text" && it->at("type")=="text","Templates only bind text content"); props[property]=format(binding.at("template"),values); }
        else { require(props.at(property).is_number(),"Expressions bind numeric properties only"); props[property]=expression(binding.at("expression"),values); }
    }
    return objects;
}
void validateDefinition(const Json& definition,const Registry& registry) {
    require(definition.at("componentDefinition").at("id").is_string(),"Definition requires stable id");
    const std::string id=definition.at("componentDefinition").at("id"); require(identifier(id),"Definition id must be an ASCII identifier");
    require(definition.at("objects").size()<=128,"A definition allows at most 128 child primitives");
    require(!definition.contains("components") || definition.at("components").empty(),"Nested components are not supported in definition v1");
    for(const auto& o:definition.at("objects")) {
        require(o.at("type")!="component","Nested component instances require a future format version");
        require(!o.contains("motion") && !o.contains("binding"),"Definition children are static local geometry; apply motion to the instance");
    }
    for(const auto& o:definition.at("objects")) require(!o.contains("animation") && !o.contains("write"),"Animate a scene instance rather than static definition children");
    for(const auto& o:materialize(definition)) registry.get(o.at("type")).validate(o.at("properties"));
}
Json instantiate(const Json& scene,const Json& instance,State position) {
    const auto& p=instance.at("properties"); const std::string id=p.at("definition");
    require(scene.contains("components") && scene.at("components").contains(id),"Component definition not installed: "+id);
    const auto& definition=scene.at("components").at(id);
    auto objects=materialize(definition,p.at("parameters"));
    const double angle=p.at("rotation"),scale=p.at("scale"),c=std::cos(angle*pi/180),s=std::sin(angle*pi/180);
    for(auto& o:objects) {
        auto& props=o["properties"]; const double x=props["x"],y=props["y"];
        props["x"]=position.x+scale*(c*x-s*y); props["y"]=position.y+scale*(s*x+c*y);
        if(props.contains("dx")) { const double dx=props["dx"],dy=props["dy"]; props["dx"]=scale*(c*dx-s*dy); props["dy"]=scale*(s*dx+c*dy); }
        if(props.contains("points")) for(auto& point:props["points"]) { const double px=point[0],py=point[1]; point={scale*(c*px-s*py),scale*(s*px+c*py)}; }
        for(const char* field:{"radius","width","height"}) if(props.contains(field)) props[field]=props[field].get<double>()*scale;
        if(o["type"]=="arc") { props["start"]=props["start"].get<double>()+angle; props["end"]=props["end"].get<double>()+angle; }
        if(o["type"]=="rectangle") props["rotation"]=props.value("rotation",0.0)+angle;
    }
    return objects;
}
Json blank(const std::string& id) {
    return {{"version",1},{"title","新组件"},{"duration",3},{"steps",4},{"objects",Json::array()},
        {"componentDefinition",{{"id",id},{"parameters",Json::object()},{"bindings",Json::array()},{"constraints",Json::array()}}}};
}
}
