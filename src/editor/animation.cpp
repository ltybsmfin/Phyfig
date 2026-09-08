#include "animation.hpp"
#include "expression.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <deque>
#include <memory>
#include <set>
#include <stdexcept>

namespace physics_editor::animation {
namespace {
void require(bool v,const std::string& message){if(!v)throw std::runtime_error(message);}
double number(const Json& v){require(v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>())<=1e6,"Animation number outside finite +/- 1000000");return v;}
bool identifier(const std::string& name){if(name.empty() || name.size()>64 || !(std::isalpha((unsigned char)name[0]) || name[0]=='_'))return false;for(unsigned char c:name)if(c>=128 || !(std::isalnum(c)||c=='_'))return false;return true;}
struct Segment {double start,end;Expression expression;};
struct Channel {std::vector<Segment> segments;Json keys;};
struct Program {
    Json parameters;
    std::map<std::string,Expression> quantities;
    std::map<std::string,Channel> channels;
    double duration;
    Program(const Json& a,double length):parameters(a.value("parameters",Json::object())),duration(length){
        require(a.is_object() && a.dump().size()<=65536,"Animation must be an object of at most 64 KB");
        require(parameters.is_object() && parameters.size()<=32,"At most 32 animation parameters");
        std::set<std::string> variables{"t","pi"};
        for(auto it=parameters.begin();it!=parameters.end();++it){require(identifier(it.key()) && it.key()!="u" && variables.insert(it.key()).second,"Invalid/reserved parameter name");number(it.value());}
        const auto q=a.value("quantities",Json::object());require(q.is_object() && q.size()<=16,"At most 16 named quantities");
        for(auto it=q.begin();it!=q.end();++it){
            require(identifier(it.key()) && it.key()!="u" && !variables.count(it.key()),"Quantity name conflicts with parameter/time");
            require(!it.value().contains("unit") || (it.value()["unit"].is_string() && it.value()["unit"].get_ref<const std::string&>().size()<=64),"Quantity unit must be a bounded string");
            quantities.emplace(it.key(),Expression(it.value().at("expression"),variables));
        }
        for(const auto& [name,_]:quantities)variables.insert(name);
        variables.insert("u");
        const auto& c=a.at("channels");require(c.is_object() && !c.empty() && c.size()<=16,"Animation requires 1..16 channels");
        for(auto it=c.begin();it!=c.end();++it){
            Channel channel;const auto& spec=it.value();const std::string kind=spec.at("kind");
            if(kind=="formula") {
                const auto& segments=spec.at("segments");require(segments.is_array() && !segments.empty() && segments.size()<=64,"Formula channel requires 1..64 contiguous segments");
                double previous=0;
                for(const auto& s:segments){const double start=number(s.at("start")),end=number(s.at("end"));require(start==previous && end>start && end<=duration,"Segments must exactly cover [0,duration] without gaps/overlap");channel.segments.push_back({start,end,Expression(s.at("expression"),variables)});previous=end;}
                require(previous==duration,"Final segment must end at document duration");
            } else if(kind=="keys") {
                channel.keys=spec.at("keys");require(channel.keys.is_array() && !channel.keys.empty() && channel.keys.size()<=512,"Key channel requires 1..512 keys");
                double previous=-1;
                for(const auto& k:channel.keys){const double time=number(k.at("time"));number(k.at("value"));require(time>=0 && time<=duration && time>previous,"Key times must be unique, ascending and within duration");previous=time;const auto mode=k.value("interpolation",std::string("linear"));require(mode=="linear" || mode=="step" || mode=="smooth","Interpolation must be linear, step or smooth");}
            } else throw std::runtime_error("Unknown channel kind: "+kind);
            channels.emplace(it.key(),std::move(channel));
        }
    }
    Json sample(double time)const{
        require(std::isfinite(time),"Animation time must be finite");time=std::clamp(time,0.0,duration);
        Json vars=parameters;vars["t"]=time;
        for(const auto& [name,expression]:quantities)vars[name]=expression.evaluate(vars);
        Json result=Json::object();
        for(const auto& [name,channel]:channels){
            double value=0;
            if(!channel.segments.empty()){
                const Segment* chosen=&channel.segments.back();for(const auto& s:channel.segments)if(time<s.end){chosen=&s;break;}
                vars["u"]=time-chosen->start;value=chosen->expression.evaluate(vars);
            }else{
                const auto& keys=channel.keys;
                if(time<=keys.front()["time"].get<double>())value=keys.front()["value"];
                else if(time>=keys.back()["time"].get<double>())value=keys.back()["value"];
                else for(size_t i=0;i+1<keys.size();++i)if(time<keys[i+1]["time"].get<double>()){
                    const double t0=keys[i]["time"],t1=keys[i+1]["time"],v0=keys[i]["value"],v1=keys[i+1]["value"];
                    double u=(time-t0)/(t1-t0);const auto mode=keys[i].value("interpolation",std::string("linear"));if(mode=="step")u=0;else if(mode=="smooth")u=u*u*(3-2*u);value=v0+(v1-v0)*u;break;
                }
            }
            require(std::isfinite(value) && std::abs(value)<=1e6,"Animated property outside finite +/- 1000000: "+name);result[name]=value;
        }
        return {{"channels",result},{"quantities",vars}};
    }
};
std::shared_ptr<const Program> compile(const Json& a,double duration){
    // Small main-thread cache; immutable entries may safely outlive eviction during sampling.
    static thread_local std::deque<std::pair<std::string,std::shared_ptr<const Program>>> cache;
    const std::string key=Json::array({duration,a}).dump();
    for(const auto& item:cache)if(item.first==key)return item.second;
    auto program=std::make_shared<Program>(a,duration);cache.emplace_back(key,program);if(cache.size()>64)cache.pop_front();return program;
}
}
Json evaluate(const Json& a,double time,double duration){return compile(a,duration)->sample(time);}
Json properties(const Json& o,double time,double duration){
    Json p=o.at("properties");if(o.contains("animation")){const auto sample=evaluate(o["animation"],time,duration);for(auto& [key,value]:sample["channels"].items())if(key!="vx" && key!="vy")p[key]=value;}
    return p;
}
void validate(const Json& o,double duration,const Registry& registry){
    if(!o.contains("animation"))return;
    require(!o.contains("motion") && !o.contains("binding"),"Authored animation cannot be combined with legacy motion/binding on the same object");
    const auto program=compile(o["animation"],duration);const auto& p=o["properties"];
    const std::set<std::string> allowed{"x","y","vx","vy","dx","dy","radius","width","height","rotation","scale","size","stroke","start","end"};
    for(const auto& [name,_]:program->channels)require(allowed.count(name) && (name=="vx" || name=="vy" || (p.contains(name) && p[name].is_number())),"Channel does not target a numeric property: "+name);
    std::set<double> times{0,duration};for(int i=1;i<32;++i)times.insert(duration*i/32);
    for(const auto& [_,channel]:program->channels){for(const auto& s:channel.segments){times.insert(s.start);times.insert(std::nextafter(s.end,s.start));}if(!channel.keys.is_null())for(const auto& key:channel.keys)times.insert(key["time"].get<double>());}
    // Sampling catches common errors, not a proof over every real-valued instant.
    // The view also validates sampled properties and pauses on runtime domain errors.
    for(double time:times)registry.get(o["type"]).validate(properties(o,time,duration));
}
Json demo(){
    auto scene=Document::demo();scene["title"]="公式驱动与瞬时跳变";scene["duration"]=6;scene["objects"]=Json::array();Registry registry;
    auto add=[&](const char* id,const char* type,const char* name,Json props){auto p=registry.get(type).defaults;p.update(props);scene["objects"].push_back({{"id",id},{"type",type},{"name",name},{"properties",p},{"visible",true},{"locked",false},{"step",0}});};
    add("ground","ground","参考地面",{{"x",-1},{"dx",14}});
    add("block","rectangle","示意物块",{{"width",.8},{"height",.8}});
    scene["objects"].back()["animation"]={{"channels",{{"x",{{"kind","formula"},{"segments",Json::array({{{"start",0},{"end",3},{"expression","0.5*t^2"}},{{"start",3},{"end",6},{"expression","4.5+2*u"}}})}}},{"y",{{"kind","keys"},{"keys",Json::array({{{"time",0},{"value",0},{"interpolation","step"}},{{"time",3},{"value",1.5},{"interpolation","step"}},{{"time",6},{"value",1.5}}})}}}}}};
    add("force","arrow","F(t) 力的示意箭头",{{"x",1},{"y",4},{"dx",0},{"dy",2},{"color",{.85,.38,.12,1}}});
    scene["objects"].back()["animation"]={{"parameters",{{"F0",4},{"omega",2}}},{"quantities",{{"F",{{"expression","F0*sin(omega*t)"},{"unit","N"}}}}},{"channels",{{"dy",{{"kind","formula"},{"segments",Json::array({{{"start",0},{"end",6},{"expression","0.5*F"}}})}}}}}};
    add("note","text","动画说明",{{"x",3},{"y",5},{"text","示意动画：t = 3 s 时高度阶跃，水平速度切换"}});
    add("force-label","text","力的映射说明",{{"x",3},{"y",4},{"text","F(t) = 4 sin(2t) N；箭头长度 = 0.5 F(t)"}});
    return scene;
}
}
