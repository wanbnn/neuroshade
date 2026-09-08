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
        const auto native=dir/"fixture.nsmodel";std::filesystem::create_directories(native);
        std::ofstream(native/"manifest.json")<<R"({"schema":1,"id":"test.nr","name":"Native fixture","version":"1","runtime":"dlssnr_hip","inputs":[{"semantic":"Color.Final","tensor":"color","dtype":"fp32","layout":"NCHW"}],"output":{"semantic":"Output.Color","tensor":"out","dtype":"fp32","layout":"NCHW"},"history":0,"scale":{"x":1,"y":1},"first_frame":"spatial","shapes":{"kind":"fixed","buckets":[{"input":[1,4,2,2],"output":[1,4,2,2]}]}})";
        for(const auto* name:{"signature.json","metadata.json"})std::ofstream(native/name)<<"{\"schema\":1}";
        for(const auto* name:{"graph.bin","preview.webp","weights.bin","kernels.hsaco","lookup.bin"})std::ofstream(native/name)<<"fixture";
        ui.draft.schema_version=2;profile::Effect nr;nr.plugin="org.neuroshade.reconstruction";nr.model=native.string();nr.strength=.65f;
        ui.draft.pipeline.insert(ui.draft.pipeline.begin(),nr);
        draw({{-1,-1,false,0xffc5}}); // F8 native controls
        draw({{710,290,true,0}}); // Output blend +
        draw({{390,290,true,0}}); // NR Intensity +
        draw({{300,255,true,0}}); // Natural
        draw({{300,255,true,0}}); // Cinematic
        draw({{300,225,true,0}}); // Preset #1
        draw({{300,225,true,0}}); // Preset #2, effective #1
        draw({{390,355,true,0}}); // Local Tone +
        draw({{270,387,true,0}}); // Local Structure -
        draw({{270,420,true,0}}); // Skin Structure -
        if(ui.draft.pipeline.front().nr_style!=2||ui.draft.pipeline.front().nr_preset!=2||ui.draft.pipeline.front().nr_intensity<1.049f||ui.draft.pipeline.front().strength<.69f||ui.draft.pipeline.front().nr_controls!=std::array<float,3>{.05f,.95f,.95f})
            throw std::runtime_error("NR controls did not change draft");
        draw({{300,320,true,0}}); // Automatic mask off
        if(ui.draft.pipeline.front().nr_auto_mask)throw std::runtime_error("NR automatic mask toggle failed");
        ui.draft.pipeline.front().nr_controls={1.54f,2.f,-1.f};
        draw({{50,500,true,0}});if(!ui.apply_requested)throw std::runtime_error("NR apply missing");ui.applied(true);
        draw({{165,500,true,0}});
        auto persisted=profile::load(dir/"game.json");
        if(!persisted.valid()||persisted.profile.pipeline.front().nr_controls!=ui.draft.pipeline.front().nr_controls||persisted.profile.pipeline.size()!=3||persisted.profile.pipeline.front().nr_auto_mask||persisted.profile.pipeline.front().nr_style!=2||persisted.profile.pipeline.front().nr_preset!=2||persisted.profile.pipeline.front().nr_intensity<1.049f)
            throw std::runtime_error("NR controls were lost in profile roundtrip");
        const auto invalid=profile::parse(R"({"schema_version":2,"game":{"executable":"fixture"},"pipeline":[{"plugin":"nr","nr_local_tone":2.01}]})");
        if(invalid.valid())throw std::runtime_error("invalid NR control accepted");
        for(const auto* field:{"\"nr_style\":3","\"nr_style\":1.5","\"nr_preset\":4","\"nr_intensity\":2.01","\"nr_intensity\":-0.1"}){
            const auto bad=profile::parse(std::string("{\"schema_version\":2,\"game\":{\"executable\":\"fixture\"},\"pipeline\":[{\"plugin\":\"nr\",")+field+"}]}");
            if(bad.valid())throw std::runtime_error("invalid NR mode accepted");
        }
        if(argc>2){
            std::ofstream image(argv[2],std::ios::binary);image<<"P6\n1920 1080\n255\n";
            for(auto color:ui.pixels()){if(!(color>>24))color=0xff0c1520;char rgb[]={char(color>>16),char(color>>8),char(color)};image.write(rgb,3);}
        }
        // Minimized/tiny extents and oversized pages must remain bounded.
        ui.draw(32,32,snapshot,{{-1,-1,false,0xff56}},false,0,false);
        if(ui.pixels().size()!=1024) throw std::runtime_error("extent mismatch");
        std::filesystem::remove_all(dir);
        std::cout<<"runtime_ui=pass text_canvas=yes effects=yes apply=yes presets=yes persistence=yes small_extent=yes\n";
    } catch(...) {std::filesystem::remove_all(dir);throw;}
}
