#include "workspace.hpp"
#include "animation.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace physics_editor;
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){try{
    Workspace w;w.dispatch({{"method","replace"},{"args",{{"document",animation::demo()}}}},"human");
    w.dispatch({{"method","open_component"},{"args",{{"id","test"}}}});
    w.dispatch({{"method","apply"},{"scope","component"},{"args",{{"commands",Json::array({{{"op","create"},{"id","circle"},{"type","circle"}}})}}}},"mcp");
    w.dispatch({{"method","close_component"},{"args",{{"discard",true}}}});
    w.dispatch({{"method","open_component"},{"args",{{"id","active"}}}});
    w.dispatch({{"method","apply"},{"scope","component"},{"args",{{"commands",Json::array({{{"op","create"},{"id","p"},{"type","point"}}})}}}});
    w.dispatch({{"method","undo"},{"scope","component"}});
    w.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","set_keyframe"},{"id","block"},{"channel","y"},{"time",3},{"value",2},{"interpolation","step"}}})}}}});
    w.dispatch({{"method","preview"},{"args",{{"revision",1}}}});
    for(int i=0;i<1002;++i)w.dispatch({{"method","playback"},{"args",{{"time",.5}}}});
    check(w.document().history()["firstAvailableId"]==1,"Audit silently truncated after 1000 operations");
    w.layout()={{"workspace","animation"},{"sidebarWidth",320}};
    auto scene=w.document().archive(),draft=w.document("component").archive();
    const auto base=std::filesystem::current_path()/"project-test";std::filesystem::create_directories(base/"moved");
    const auto path=(base/"original.physproj").u8string(),copy=(base/"moved"/"portable.physproj").u8string();
    w.dispatch({{"method","save_project"},{"args",{{"path",path},{"overwrite",true}}}});
    std::filesystem::copy_file(std::filesystem::u8path(path),std::filesystem::u8path(copy),std::filesystem::copy_options::overwrite_existing);
    Workspace restored;restored.dispatch({{"method","load_project"},{"args",{{"path",copy}}}});
    check(restored.document().archive()==scene,"Scene history/archive lost on moved project load");
    check(restored.document("component").archive()==draft,"Component history/redo not preserved");
    check(restored.projectInfo()["closedDraftCount"]==1,"Closed draft history lost");
    restored.dispatch({{"method","preview"}});restored.dispatch({{"method","undo"}});
    check(restored.document().evaluate("block",3).y==1.5,"Reopened undo did not restore animation");
    restored.dispatch({{"method","redo"},{"scope","component"}});check(restored.document("component").scene()["objects"].size()==1,"Draft redo after reload failed");
    auto before=restored.document().archive();Json corrupt;{std::ifstream in(std::filesystem::u8path(copy));in>>corrupt;}
    corrupt["component"]["head"]=999999;const auto bad=(base/"bad.physproj").u8string();{std::ofstream out(std::filesystem::u8path(bad));out<<corrupt;}
    bool rejected=false;try{restored.dispatch({{"method","load_project"},{"args",{{"path",bad},{"discard",true}}}});}catch(...){rejected=true;}
    check(rejected && restored.document().archive()==before,"Corrupt draft changed live scene during load");
    auto token=restored.projectInfo()["token"];restored.dispatch({{"method","playback"},{"args",{{"time",1}}}});
    rejected=false;try{restored.dispatch({{"method","save_project"},{"args",{{"path",copy},{"overwrite",true},{"expectedToken",token}}}},"mcp");}catch(...){rejected=true;}
    check(rejected,"Stale MCP project token accepted");
    Workspace oldLibrary;
    const Json mask={{"name","old"},{"width",1},{"height",1},{"rle",{1,255}}};
    oldLibrary.resources()={{"formulaScale",.3},{"formulaMasks",Json::array({mask})}};
    const auto legacy=(base/"legacy-library.physproj").u8string();
    oldLibrary.dispatch({{"method","save_project"},{"args",{{"path",legacy},{"overwrite",true}}}});
    Workspace newLibrary;auto extra=mask;extra["name"]="new";
    newLibrary.resources()={{"formulaScale",.3},{"formulaMasks",Json::array({mask,extra})}};
    newLibrary.dispatch({{"method","load_project"},{"args",{{"path",legacy}}}});
    check(newLibrary.resources()["formulaMasks"].size()==2,"Old project replaced installed library; next save would omit new assets");
    const auto newBefore=newLibrary.document().archive();
    Json incompatible;{std::ifstream in(std::filesystem::u8path(legacy));in>>incompatible;}
    incompatible["resources"]["formulaMasks"][0]["rle"]={1,128};
    {std::ofstream out(std::filesystem::u8path(bad));out<<incompatible;}
    rejected=false;try{newLibrary.dispatch({{"method","load_project"},{"args",{{"path",bad},{"discard",true}}}});}catch(...){rejected=true;}
    check(rejected && newLibrary.document().archive()==newBefore,"Changed formula mask silently accepted or replaced live document");
    incompatible["resources"]=oldLibrary.resources();incompatible["layout"]["referenceZoom"]="invalid";
    {std::ofstream out(std::filesystem::u8path(bad));out<<incompatible;}
    rejected=false;try{newLibrary.dispatch({{"method","load_project"},{"args",{{"path",bad},{"discard",true}}}});}catch(...){rejected=true;}
    check(rejected && newLibrary.document().archive()==newBefore,"Malformed zoom layout escaped staged validation");
    std::cout<<"PASS: portable project, scope archives, closed drafts, preview, continued undo/redo, atomic corruption rejection, concurrency token\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
