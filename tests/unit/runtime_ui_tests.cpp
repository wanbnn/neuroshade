#include "overlay/runtime_ui.hpp"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
using namespace neuroshade;
int main(int argc,char** argv) {
    const auto dir=std::filesystem::temp_directory_path()/("ns-ui-test-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    try {
        profile::Profile profile;profile.executable="test-game";
        profile.pipeline={{"org.neuroshade.sharpen",true,0.45f},{"org.neuroshade.color_adjust",true,0.3f}};
        std::string error;
        if(!profile::save(profile,dir/"game.json",error)) throw std::runtime_error(error);
        overlay::RuntimeUi ui(profile,dir/"game.json",dir);
        overlay::Snapshot snapshot; snapshot.game="Far Cry 3";snapshot.gpu="AMD Radeon RX 9060 XT";
        auto draw=[&](std::vector<overlay::UiInput> events={}){ui.tick();ui.draw(1920,1080,snapshot,events,true,1.25,true);};
        draw();
        if(argc>1) {
            std::ofstream image(argv[1],std::ios::binary);image<<"P6\n1920 1080\n255\n";
            for(auto color:ui.pixels()) {if(!(color>>24)) color=0xff0c1520; char rgb[]={char(color>>16),char(color>>8),char(color)};image.write(rgb,3);}
        }
        draw({{270,215,true,0}});
        if(ui.draft.pipeline[0].enabled) throw std::runtime_error("toggle did not modify pipeline");
        draw({{472,215,true,0}});
        if(ui.draft.pipeline[0].strength<0.49f) throw std::runtime_error("strength did not change");
        draw({{50,500,true,0}});
        if(!ui.apply_requested) throw std::runtime_error("apply request missing");
        ui.applied(true);
        draw({{165,500,true,0}});
        const auto saved=profile::load(dir/"game.json");
        if(!saved.valid() || saved.profile.pipeline[0].enabled || saved.profile.pipeline[0].strength<0.49f)
            throw std::runtime_error("profile save roundtrip failed");
        draw({{-1,-1,false,0xffbf}}); // F2 presets
        draw({{290,500,true,0}}); // save new
        if(!std::filesystem::exists(dir/"presets") || std::filesystem::is_empty(dir/"presets"))
            throw std::runtime_error("preset creation failed");
        draw({{-1,-1,false,0xffc2}}); // F5 settings
        draw({{45,200,true,0}});
        if(!std::filesystem::exists(dir/"game.json.overlay")) throw std::runtime_error("settings not persisted");
        // Minimized/tiny extents and oversized pages must remain bounded.
        ui.draw(32,32,snapshot,{{-1,-1,false,0xff56}},false,0,false);
        if(ui.pixels().size()!=1024) throw std::runtime_error("extent mismatch");
        std::filesystem::remove_all(dir);
        std::cout<<"runtime_ui=pass text_canvas=yes effects=yes apply=yes presets=yes persistence=yes small_extent=yes\n";
    } catch(...) {std::filesystem::remove_all(dir);throw;}
}
