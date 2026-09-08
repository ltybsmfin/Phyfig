#include "document.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <fstream>

using namespace physics_editor;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) { bool rejected=false; try { action(); } catch(const std::exception&) { rejected=true; } check(rejected,"Expected operation to reject"); }
int main() {
    try {
        Document d;
        const auto initial=d.snapshot();
        rejects([&]{d.dispatch({{"method","apply"},{"args",{{"commands",Json::array({
            {{"op","move"},{"id","ball"},{"dx",2},{"dy",0}},{{"op","delete"},{"id","missing"}}})}}}});});
        check(d.snapshot()==initial,"Failed batch changed scene/history/playback");
        rejects([&]{d.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","delete"},{"id","ball"}}})}}}});});
        check(d.snapshot()==initial,"Dangling reference deletion changed document");
        d.dispatch({{"method","apply"},{"expectedRevision",0},{"args",{{"commands",Json::array({{{"op","move"},{"id","ball"},{"dx",2},{"dy",0}}})}}}});
        rejects([&]{d.dispatch({{"method","undo"},{"expectedRevision",0}});});
        d.dispatch({{"method","undo"}}); check(d.scene()==initial["document"],"Undo failed");
        d.dispatch({{"method","redo"}}); check(d.object("ball")["properties"]["x"]==2,"Redo failed");
        check(std::abs(d.evaluate("ball",15.0/9.8).vy)<1e-10,"Apex velocity incorrect");
        check(std::abs(d.evaluate("ball",30.0/9.8).y)<1e-10,"Landing height incorrect");
        check(d.evaluate("velocity",1).x==d.evaluate("ball",1).x,"Velocity binding not following target");
        auto malformed=d.scene(); malformed["objects"][0]["properties"]["stroke"]=-1;
        rejects([&]{d.dispatch({{"method","replace"},{"args",{{"document",malformed}}}});});
        auto cycle=d.scene(); cycle["objects"][1].erase("motion"); cycle["objects"][1]["binding"]={{"target","velocity"},{"kind","position"},{"scale",1}};
        rejects([&]{d.dispatch({{"method","replace"},{"args",{{"document",cycle}}}});});
        const auto path=std::filesystem::current_path()/std::filesystem::u8path("编辑器往返测试.json");
        d.dispatch({{"method","save"},{"args",{{"path",path.u8string()},{"overwrite",true}}}});
        Document loaded; loaded.dispatch({{"method","load"},{"args",{{"path",path.u8string()}}}}); check(loaded.scene()==d.scene(),"UTF-8 save/load roundtrip failed");
        rejects([&]{loaded.dispatch({{"method","save"},{"args",{{"path",path.u8string()}}}});}); std::filesystem::remove(path);
        Registry registry; registry.add("custom",{"Extension",{{"x",0},{"y",0}},[](const Json& p){check(p.contains("x"),"Custom validation missing x");}});
        Document extended(std::move(registry));
        extended.registerCommand("rename_all","Example extension command",[](Json& s,const Json&){for(auto& o:s["objects"]) o["name"]="extension";});
        extended.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","create"},{"id","custom"},{"type","custom"}},{{"op","rename_all"}}})}}}});
        check(extended.object("custom")["name"]=="extension","Registry extension failed");
        extended.dispatch({{"method","undo"}}); check(extended.scene()==Document::demo(),"Extension batch undo failed");
        Document audit;
        audit.startJournal((std::filesystem::current_path()/"journal-test").u8string());
        const auto edit=Json{{"method","apply"},{"args",{{"commands",Json::array({{{"op","move"},{"id","ball"},{"dx",1},{"dy",0}}})}}}};
        audit.dispatch(edit,"human"); audit.dispatch(edit,"mcp");
        audit.dispatch({{"method","undo"}},"human");
        auto log=audit.history(); check(log["entries"].size()==3,"Wrong event count");
        check(log["entries"][0]["source"]=="human" && log["entries"][1]["source"]=="mcp","Input source missing");
        check(log["entries"][2]["relatedEvent"]==2,"Cross-source undo target incorrect");
        audit.dispatch({{"method","redo"}},"mcp");
        check(audit.history()["entries"].back()["relatedEvent"]==2,"Redo target incorrect");
        rejects([&]{audit.dispatch({{"method","undo"},{"expectedRevision",0}},"mcp");});
        check(audit.history()["entries"].back()["status"]=="rejected","Rejected edit not recorded");
        const auto count=audit.history()["lastId"];
        audit.dispatch({{"method","get_scene"}},"mcp"); audit.advance(.01);
        check(audit.history()["lastId"]==count,"Reads/animation polluted operation log");
        auto page=audit.history(1,2); check(page["entries"].size()==2 && page["nextCursor"]==3 && page["hasMore"]==true,"History pagination failed");
        const auto journal=std::filesystem::u8path(log["journalPath"].get<std::string>());
        std::ifstream stream(journal); std::string line; size_t records=0;
        while(std::getline(stream,line)) { auto event=Json::parse(line); if(records++==0) check(event["initialState"]["document"]==Document::demo(),"Journal baseline missing"); }
        check(records==6,"Journal did not persist all operations"); stream.close(); std::filesystem::remove(journal);
        std::cout<<"Document tests passed: atomic batches, revisions, history, motion, references, validation, UTF-8 persistence, registry extensions, shared source-aware journal, undo links, pagination\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
