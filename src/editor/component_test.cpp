#include "workspace.hpp"
#include "component.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace physics_editor;
namespace {
void check(bool value,const char* error) { if(!value) throw std::runtime_error(error); }
void near(double a,double b) { check(std::abs(a-b)<1e-8,"Geometry invariant failed"); }
template<class F> void rejects(F action) { bool failed=false; try {action();}catch(const std::exception&){failed=true;} check(failed,"Invalid operation accepted"); }
Json command(Json edits,const std::string& scope="scene") {return {{"method","apply"},{"scope",scope},{"args",{{"commands",edits}}}};}
Json object(const Json& objects,const std::string& id) { for(const auto& o:objects) if(o["id"]==id) return o; throw std::runtime_error("Missing child: "+id); }
}
int main() {
    try {
        std::ifstream file("scenes/incline-arc.json"); Json scene; file>>scene;
        const auto definition=scene["components"]["incline_arc"];
        for(double radius:{.1,.3,.8}) for(double angle:{20.,37.,65.}) {
            auto children=components::materialize(definition,{{"R",radius},{"angle",angle},{"L",3}});
            const auto track=object(children,"track")["properties"],ab=object(children,"smooth")["properties"],bc=object(children,"rough")["properties"];
            const double cx=bc["x"].get<double>()+bc["dx"].get<double>(),cy=bc["y"].get<double>()+bc["dy"].get<double>();
            near(std::hypot(cx,cy),radius); near(cx*bc["dx"].get<double>()+cy*bc["dy"].get<double>(),0);
            near(bc["y"],0); near(ab["x"].get<double>()+ab["dx"].get<double>(),bc["x"]);
            near(ab["y"].get<double>()+ab["dy"].get<double>(),0); near(track["radius"],radius);
        }
        rejects([&]{components::materialize(definition,{{"L",.1},{"R",1}});});
        rejects([&]{components::materialize(definition,{{"unknown",1}});});
        rejects([&]{components::materialize(definition,{{"R",-1}});});
        auto invalidUnit=definition; invalidUnit["componentDefinition"]["parameters"]["R"]["unit"]=42;
        rejects([&]{components::materialize(invalidUnit);});
        near(components::expression("sin(30)*4 + sqrt(9) - abs(-2)",Json::object()),3);
        for(const std::string& expression:std::vector<std::string>{"1/0","sqrt(-1)","system(1)","R.x","1e999","1;2",std::string(40,'(')+"1"+std::string(40,')')})
            rejects([&]{components::expression(expression,{{"R",1}});});
        auto instance=scene["objects"][0]; instance["properties"]["rotation"]=90; instance["properties"]["scale"]=2;
        const auto transformed=components::instantiate(scene,instance,{2,3,0,0});
        const auto arc=object(transformed,"track")["properties"]; near(arc["x"],2);near(arc["y"],3);near(arc["radius"],.6);near(arc["start"],323);
        Workspace w; w.dispatch({{"method","replace"},{"args",{{"document",scene}}}},"mcp");
        const auto mainBefore=w.document().snapshot(),historyBefore=w.document().history();
        w.dispatch({{"method","open_component"},{"expectedRevision",w.document().revision()},{"args",{{"id","incline_arc"}}}},"human");
        w.dispatch(command(Json::array({{{"op","component_settings"},{"patch",{{"parameters",{{"R",{{"default",.4}}}}}}}}}),"component"),"mcp");
        check(w.document().snapshot()==mainBefore && w.document().history()==historyBefore,"Draft polluted scene history");
        const auto draftBefore=w.document("component").snapshot();
        w.dispatch(command(Json::array({{{"op","move"},{"id","apparatus"},{"dx",2},{"dy",1}}})),"human");
        check(w.document("component").snapshot()==draftBefore,"Scene edit affected draft");
        w.dispatch({{"method","undo"}},"mcp");
        const auto count=w.document().history()["lastId"].get<int>();
        w.dispatch({{"method","publish_component"},{"expectedRevision",w.document().revision()},{"args",{{"componentRevision",w.document("component").revision()}}}},"human");
        check(w.document().history()["lastId"].get<int>()==count+1,"Publish is not one transaction");
        check(w.document("component").snapshot()==draftBefore,"Publish consumed draft history");
        near(components::instantiate(w.document().scene(),w.document().object("apparatus"),{0,0,0,0})[2]["properties"]["radius"],.4);
        w.dispatch(command(Json::array({{{"op","create"},{"type","component"},{"id","copy"},{"properties",{{"definition","incline_arc"},{"parameters",{{"R",.6}}},{"x",4},{"rotation",90},{"scale",2}}}}})),"mcp");
        const auto copy=components::instantiate(w.document().scene(),w.document().object("copy"),{4,0,0,0}); near(object(copy,"track")["properties"]["radius"],1.2);
        const auto head=w.document().snapshot();
        rejects([&]{w.dispatch(command(Json::array({{{"op","component_settings"},{"patch",{{"parameters",{{"R",{{"min",1}}}}}}}}}),"component"));});
        check(w.document().snapshot()==head,"Rejected draft transaction changed scene");
        const auto eventCount=w.document().history()["lastId"];
        w.dispatch({{"method","preview"},{"args",{{"revision",1}}}});
        near(w.document().scene()["components"]["incline_arc"]["componentDefinition"]["parameters"]["R"]["default"],.3);
        check(w.document().snapshot()["document"]==head["document"],"Scrubbing mutated head");
        check(w.document().history()["lastId"]==eventCount,"Scrubbing polluted journal");
        rejects([&]{w.dispatch({{"method","undo"}});});
        w.dispatch({{"method","restore_preview"},{"expectedRevision",w.document().revision()}},"mcp");
        check(w.document().history()["entries"].back()["restoredRevision"]==1,"Restore audit omitted selected checkpoint");
        check(!w.document().previewing() && w.document().scene()==scene,"Restore did not produce correct scene");
        w.dispatch({{"method","undo"}},"human"); check(w.document().scene()==head["document"],"History restore is not undoable");
        w.dispatch({{"method","preview"},{"scope","component"},{"args",{{"revision",0}}}});
        check(w.document("component").previewing() && !w.document().previewing(),"Preview scopes mixed");
        w.dispatch({{"method","preview"},{"scope","component"}});
        rejects([&]{w.dispatch({{"method","close_component"}});});
        w.dispatch({{"method","close_component"},{"args",{{"discard",true}}}}); check(!w.hasComponent(),"Close failed");
        Document retained;
        for(int i=0;i<110;++i) retained.dispatch(command(Json::array({{{"op","move"},{"id","ball"},{"dx",1},{"dy",0}}})));
        check(retained.timeline()["states"].size()==101,"Checkpoint retention unbounded");
        rejects([&]{retained.dispatch({{"method","preview"},{"args",{{"revision",0}}}});});
        std::cout<<"PASS: tangent/height invariants, constrained parameters, instance transforms, independent histories, publish transaction, immutable preview, undoable restore, bounded retention\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
