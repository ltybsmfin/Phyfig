#include "expression.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace physics_editor {
struct Expression::Node {
    std::string op;
    double value=0;
    std::vector<std::shared_ptr<const Node>> args;
};
namespace {
using Node=Expression::Node;
void require(bool valid,const std::string& message) { if(!valid) throw std::runtime_error(message); }
class Parser {
    const std::string& source_; const std::set<std::string>& variables_;
    size_t pos_=0,nodes_=0; unsigned depth_=0;
    void spaces() {while(pos_<source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_]))) ++pos_;}
    bool take(char c) {spaces(); if(pos_<source_.size() && source_[pos_]==c){++pos_;return true;}return false;}
    std::shared_ptr<const Node> node(std::string op,std::vector<std::shared_ptr<const Node>> args={},double value=0) {
        require(++nodes_<=256,"Expression exceeds 256 nodes");return std::make_shared<Node>(Node{std::move(op),value,std::move(args)});
    }
    std::shared_ptr<const Node> primary() {
        require(++depth_<=32,"Expression nesting exceeds 32"); std::shared_ptr<const Node> result;
        if(take('(')){result=sum();require(take(')'),"Missing closing parenthesis");}
        else {
            spaces();require(pos_<source_.size(),"Unexpected end of expression");
            if(std::isdigit(static_cast<unsigned char>(source_[pos_])) || source_[pos_]=='.') {
                char* end=nullptr; const double v=std::strtod(source_.c_str()+pos_,&end);
                require(end!=source_.c_str()+pos_ && std::isfinite(v),"Invalid number"); pos_=size_t(end-source_.c_str());result=node("number",{},v);
            } else {
                const auto begin=pos_;
                while(pos_<source_.size() && (std::isalnum(static_cast<unsigned char>(source_[pos_])) || source_[pos_]=='_'))++pos_;
                const auto name=source_.substr(begin,pos_-begin);require(!name.empty() && name.size()<=64,"Invalid identifier");
                if(take('(')) {
                    std::vector<std::shared_ptr<const Node>> args; args.push_back(sum());
                    while(take(',')) {require(args.size()<3,"Too many function arguments");args.push_back(sum());}
                    require(take(')'),"Missing function parenthesis");
                    const size_t count=(name=="min" || name=="max" || name=="pow")?2:name=="clamp"?3:1;
                    require(std::set<std::string>{"sin","cos","tan","sind","cosd","sqrt","abs","exp","log","min","max","pow","clamp"}.count(name)>0,"Unknown function: "+name);
                    require(args.size()==count,"Wrong argument count: "+name);result=node(name,std::move(args));
                } else {require(name=="pi" || variables_.count(name)>0,"Unknown variable: "+name);result=node("$"+name);}
            }
        }
        --depth_;return result;
    }
    std::shared_ptr<const Node> unary() {
        require(++depth_<=32,"Expression nesting exceeds 32"); std::shared_ptr<const Node> v;
        if(take('+')) v=unary();else if(take('-'))v=node("neg",{unary()});else v=power(); --depth_; return v;
    }
    std::shared_ptr<const Node> power() {auto v=primary();if(take('^'))v=node("pow",{v,unary()});return v;}
    std::shared_ptr<const Node> product() {auto v=unary();for(;;){if(take('*'))v=node("*",{v,unary()});else if(take('/'))v=node("/",{v,unary()});else return v;}}
    std::shared_ptr<const Node> sum() {auto v=product();for(;;){if(take('+'))v=node("+",{v,product()});else if(take('-'))v=node("-",{v,product()});else return v;}}
public:
    Parser(const std::string& source,const std::set<std::string>& variables):source_(source),variables_(variables){}
    std::shared_ptr<const Node> run(){require(!source_.empty() && source_.size()<=1024,"Expression length must be 1..1024 bytes");auto v=sum();spaces();require(pos_==source_.size(),"Unexpected expression suffix");return v;}
};
double evaluate(const Node& n,const nlohmann::json& vars) {
    double v=0;
    if(n.op=="number")v=n.value;
    else if(n.op[0]=='$') { const auto name=n.op.substr(1); if(name=="pi")v=3.14159265358979323846;else {require(vars.contains(name) && vars[name].is_number(),"Missing variable: "+name);v=vars[name].get<double>();} }
    else {
        const double a=evaluate(*n.args[0],vars),b=n.args.size()>1?evaluate(*n.args[1],vars):0;
        if(n.op=="+")v=a+b;else if(n.op=="-")v=a-b;else if(n.op=="*")v=a*b;
        else if(n.op=="/"){require(b!=0,"Division by zero");v=a/b;}
        else if(n.op=="neg")v=-a;else if(n.op=="pow")v=std::pow(a,b);
        else if(n.op=="sin")v=std::sin(a);else if(n.op=="cos")v=std::cos(a);else if(n.op=="tan")v=std::tan(a);
        else if(n.op=="sind")v=std::sin(a*3.14159265358979323846/180);else if(n.op=="cosd")v=std::cos(a*3.14159265358979323846/180);
        else if(n.op=="sqrt")v=std::sqrt(a);else if(n.op=="abs")v=std::abs(a);else if(n.op=="exp")v=std::exp(a);else if(n.op=="log")v=std::log(a);
        else if(n.op=="min")v=std::min(a,b);else if(n.op=="max")v=std::max(a,b);
        else if(n.op=="clamp") {const double c=evaluate(*n.args[2],vars);require(b<=c,"clamp minimum exceeds maximum");v=std::clamp(a,b,c);}
    }
    require(std::isfinite(v) && std::abs(v)<=1e12,"Expression domain error or result outside +/- 1e12");return v;
}
}
Expression::Expression(const std::string& source,const std::set<std::string>& variables):root_(Parser(source,variables).run()){}
double Expression::evaluate(const nlohmann::json& variables)const{return physics_editor::evaluate(*root_,variables);}
}
