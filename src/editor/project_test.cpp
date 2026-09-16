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
    oldLibrary.setResources({{"formulaScale",.3},{"formulaMasks",Json::array({mask})}});
    const auto legacy=(base/"legacy-library.physproj").u8string();
    oldLibrary.dispatch({{"method","save_project"},{"args",{{"path",legacy},{"overwrite",true}}}});
    Workspace newLibrary;auto extra=mask;extra["name"]="new";
    newLibrary.setResources({{"formulaScale",.3},{"formulaMasks",Json::array({mask,extra})}});
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
    // Formula writing plans are additive metadata, not bitmap content: a project
    // saved before plans existed must still load against a library that has one,
    // and the installed plan is what the reloaded project uses.
    {
        auto findAsset=[](const Json& resources,const std::string& name)->const Json&{
            for(const auto& asset:resources.at("formulaMasks"))if(asset.at("name")==name)return asset;
            throw std::runtime_error("Missing formula asset "+name);
        };
        const Json plan={{"version",1},{"width",1},{"height",1},{"componentCount",1},
            {"units",Json::array({Json{{"kind","glyph"},{"box",{0,0,1,1}},{"components",{0}}}})}};
        Workspace plannedLibrary;
        auto planned=mask;planned["writingPlan"]=plan;
        plannedLibrary.setResources({{"formulaScale",.3},{"formulaMasks",Json::array({planned})}});
        plannedLibrary.dispatch({{"method","load_project"},{"args",{{"path",legacy}}}});
        check(plannedLibrary.resources()["formulaMasks"].size()==1,"Loading an old project dropped or duplicated its assets");
        check(findAsset(plannedLibrary.resources(),"old")==planned,
            "An old project without a writing plan did not adopt the installed one");
        const auto plannedPath=(base/"planned-library.physproj").u8string();
        plannedLibrary.dispatch({{"method","save_project"},{"args",{{"path",plannedPath},{"overwrite",true}}}});
        // The other direction: a project that carries a plan must load against a
        // library that does not have one, and must not lose the plan.
        Workspace plainLibrary;
        plainLibrary.setResources({{"formulaScale",.3},{"formulaMasks",Json::array({mask})}});
        plainLibrary.dispatch({{"method","load_project"},{"args",{{"path",plannedPath}}}});
        check(findAsset(plainLibrary.resources(),"old").contains("writingPlan"),
            "A writing plan stored by the project was dropped by a plan-less library");
        // A malformed or mismatched plan is still rejected: stored data is never trusted.
        for(const Json& broken:Json::array({
                Json{{"version",1},{"width",1},{"height",1},{"componentCount",1},{"units",Json::array({Json{{"components",{0,1}}}})}},
                Json{{"version",1},{"width",2},{"height",1},{"componentCount",1},{"units",Json::array({Json{{"components",{0}}}})}},
                Json{{"version",2},{"width",1},{"height",1},{"componentCount",1},{"units",Json::array({Json{{"components",{0}}}})}},
                Json{{"version",1},{"width",1},{"height",1},{"componentCount",1},{"units",Json::array()}}})){
            Json malicious;{std::ifstream in(std::filesystem::u8path(legacy));in>>malicious;}
            malicious["resources"]["formulaMasks"][0]["writingPlan"]=broken;
            {std::ofstream out(std::filesystem::u8path(bad));out<<malicious;}
            const auto beforeLoad=plainLibrary.document().archive();
            rejected=false;try{plainLibrary.dispatch({{"method","load_project"},{"args",{{"path",bad},{"discard",true}}}});}catch(...){rejected=true;}
            check(rejected && plainLibrary.document().archive()==beforeLoad,"Malformed writing plan escaped validation");
        }
    }
    // A4/U2: layout is persisted but excluded from the concurrency token, while
    // resources stay covered so a background asset cannot be lost silently.
    {
        Workspace layoutWorkspace;
        layoutWorkspace.layout()={{"workspace","animation"},{"sidebarWidth",9999},{"bottomHeight",260},{"historyOpen",true},
            {"grid",true},{"trails",false},{"followMcp",true},{"scope","scene"},{"selected","ball"},{"channel","x"},
            {"zoom",55},{"pan",{12.5,-3.5}},{"referenceZoom",55}};
        const auto layoutToken=layoutWorkspace.projectInfo()["token"];
        layoutWorkspace.layout()["zoom"]=800;
        check(layoutToken==layoutWorkspace.projectInfo()["token"],"Layout change altered the project concurrency token");
        check(!layoutWorkspace.projectDirty(),"Layout change made an unsaved project dirty");
        const auto layoutPath=(base/"layout.physproj").u8string();
        layoutWorkspace.dispatch({{"method","save_project"},{"args",{{"path",layoutPath},{"overwrite",true}}}});
        check(!layoutWorkspace.projectDirty(),"Saving a layout-only project left it dirty");
        layoutWorkspace.layout()["selected"]="other";
        check(!layoutWorkspace.projectDirty(),"Selection change made the project dirty");
        layoutWorkspace.dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","move"},{"id","ball"},{"dx",1},{"dy",0}}})}}}});
        check(layoutWorkspace.projectDirty(),"Document edit did not make the project dirty");
        auto scaleResources=layoutWorkspace.resources();scaleResources["formulaScale"]=0.5;layoutWorkspace.setResources(scaleResources);
        const auto resourceToken=layoutWorkspace.projectInfo()["token"];
        scaleResources=layoutWorkspace.resources();scaleResources["formulaScale"]=0.75;layoutWorkspace.setResources(scaleResources);
        check(resourceToken!=layoutWorkspace.projectInfo()["token"],"Resource change escaped the project token");
        scaleResources=layoutWorkspace.resources();scaleResources["formulaScale"]=0.5;layoutWorkspace.setResources(scaleResources);
        check(resourceToken==layoutWorkspace.projectInfo()["token"],"Restoring a resource did not restore the token");
        layoutWorkspace.dispatch({{"method","save_project"},{"args",{{"path",layoutPath},{"overwrite",true}}}});
        Workspace layoutRestored;
        layoutRestored.dispatch({{"method","load_project"},{"args",{{"path",layoutPath}}}});
        check(!layoutRestored.projectDirty(),"Loaded layout was reported dirty");
        check(layoutRestored.layout()==layoutWorkspace.layout(),"Layout was not preserved across save/load");
        // applyLayout clamps presentation values after load; normalization must
        // not look like a user edit.
        layoutRestored.layout()["sidebarWidth"]=700;
        check(!layoutRestored.projectDirty(),"Editor layout normalization made the loaded project dirty");
    }
    // Task 1: setResources commits the whole library, the fingerprint covers
    // RLE/latex/extension fields, a read-only traversal never dirties the
    // project, and empty containers/null cannot collide.
    {
        Workspace maskWorkspace;
        const Json maskAsset={{"name","mask"},{"width",2},{"height",1},{"rle",{1,10,1,20}},{"latex","x"}};
        maskWorkspace.setResources({{"formulaScale",.3},{"formulaMasks",Json::array({maskAsset})}});
        const auto maskToken=maskWorkspace.projectInfo()["token"];
        for(int i=0;i<3;++i)
            check(maskWorkspace.resources()["formulaMasks"][0]["name"]=="mask","Read-only library access failed");
        check(maskToken==maskWorkspace.projectInfo()["token"],"Read-only resource access changed the token");
        check(!maskWorkspace.projectDirty(),"Read-only resource access made the project dirty");
        // Same name and dimensions, RLE payload changed: the token must change.
        auto changed=maskWorkspace.resources();changed["formulaMasks"][0]["rle"]={1,11,1,20};maskWorkspace.setResources(changed);
        check(maskToken!=maskWorkspace.projectInfo()["token"],"Same-name same-size RLE change escaped the project token");
        auto maskRestoredResources=maskWorkspace.resources();maskRestoredResources["formulaMasks"][0]["rle"]={1,10,1,20};maskWorkspace.setResources(maskRestoredResources);
        check(maskToken==maskWorkspace.projectInfo()["token"],"Restoring the RLE payload did not restore the token");
        check(maskWorkspace.projectInfo()["token"]==maskWorkspace.projectInfo()["token"],"Repeated token query was not stable");
        auto latex=maskWorkspace.resources();latex["formulaMasks"][0]["latex"]="y";maskWorkspace.setResources(latex);
        check(maskToken!=maskWorkspace.projectInfo()["token"],"latex change escaped the project token");
        maskWorkspace.setResources(maskRestoredResources);
        auto maskExtra=maskWorkspace.resources();maskExtra["formulaMasks"][0]["extra"]={{"k",1}};maskWorkspace.setResources(maskExtra);
        check(maskToken!=maskWorkspace.projectInfo()["token"],"Unknown resource field escaped the project token");
        maskWorkspace.setResources(maskRestoredResources);
        check(maskToken==maskWorkspace.projectInfo()["token"],"Removing the unknown field did not restore the token");
        Workspace emptyWorkspace;
        emptyWorkspace.setResources({{"formulaScale",.3},{"formulaMasks",Json::array()},{"meta",Json::array()}});
        const auto emptyToken=emptyWorkspace.projectInfo()["token"];
        auto objectMeta=emptyWorkspace.resources();objectMeta["meta"]=Json::object();emptyWorkspace.setResources(objectMeta);
        check(emptyToken!=emptyWorkspace.projectInfo()["token"],"Empty array and empty object collided in the fingerprint");
        auto nullMeta=emptyWorkspace.resources();nullMeta["meta"]=nullptr;emptyWorkspace.setResources(nullMeta);
        check(emptyToken!=emptyWorkspace.projectInfo()["token"],"Empty array and null collided in the fingerprint");
        const auto maskPath=(base/"mask-token.physproj").u8string();
        maskWorkspace.dispatch({{"method","save_project"},{"args",{{"path",maskPath},{"overwrite",true}}}});
        check(!maskWorkspace.projectDirty(),"Mask project stayed dirty after save");
        Workspace maskRestored;
        maskRestored.dispatch({{"method","load_project"},{"args",{{"path",maskPath}}}});
        check(!maskRestored.projectDirty(),"Reloaded mask project was reported dirty");
        check(maskRestored.resources()==maskWorkspace.resources(),"Reloaded mask library differs from the saved one");
    }
    std::cout<<"PASS: portable project, scope archives, closed drafts, preview, continued undo/redo, atomic corruption rejection, concurrency token\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
