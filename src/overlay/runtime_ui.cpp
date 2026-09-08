#include "overlay/runtime_ui.hpp"
#include "overlay/font/glyphs.hpp"
#include "neural/model/package.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>

namespace neuroshade::overlay {
namespace {
std::string number(double value,int precision=1) {
    if (value<0 || !std::isfinite(value)) return "N/D";
    std::ostringstream out; out<<std::fixed<<std::setprecision(precision)<<value; return out.str();
}
double read_number(const std::filesystem::path& path,double scale=1) {
    double value{}; std::ifstream f(path); return (f>>value) ? value/scale : -1;
}
std::string read_line(const std::filesystem::path& path) { std::ifstream f(path); std::string s; std::getline(f,s); return s; }
std::string pci_name(const std::filesystem::path& device) {
    auto vendor=read_line(device/"vendor"), id=read_line(device/"device");
    if(vendor.size()<6 || id.size()<6) return {};
    vendor=vendor.substr(2);id=id.substr(2);
    for(const auto* path:{"/usr/share/hwdata/pci.ids","/run/host/usr/share/hwdata/pci.ids","/usr/share/misc/pci.ids"}) {
        std::ifstream f(path);std::string line;bool in_vendor=false;
        while(std::getline(f,line)) {
            if(line.empty() || line[0]=='#') continue;
            if(line[0]!='\t') in_vendor=line.starts_with(vendor+" ");
            else if(in_vendor && line.starts_with("\t"+id+" ")) {
                auto start=line.find_first_not_of(" \t",5);
                return start==std::string::npos?id:line.substr(start);
            }
        }
    }
    return {};
}
std::string short_id(const std::string& id) { auto p=id.rfind('.'); return p==std::string::npos?id:id.substr(p+1); }
}
RuntimeUi::RuntimeUi(profile::Profile profile,std::filesystem::path path,std::filesystem::path root)
    : draft(std::move(profile)),path_(std::move(path)),root_(std::move(root)),
      presets_dir_(path_.parent_path()/"presets"),defaults_(draft) {
    std::ifstream settings(path_.string()+".overlay");
    int fps=1,profiler=1; if(settings>>fps>>profiler) {show_fps_=fps!=0;show_profiler_=profiler!=0;}
    scan();
}
void RuntimeUi::scan() {
    presets_.clear(); models_.clear();native_models_.clear();
    std::error_code ec;
    for (const auto& dir : {path_.parent_path(),presets_dir_})
        for (const auto& entry : std::filesystem::directory_iterator(dir,ec))
            if (entry.path().extension()==".json") {
                const auto p=profile::load(entry.path());
                if (p.valid() && p.profile.executable==draft.executable) presets_.push_back(entry.path());
            }
    ec.clear();
    for (const auto& entry : std::filesystem::directory_iterator(root_/"share/neuroshade/models",ec))
        if (entry.path().extension()==".nsmodel") models_.push_back(entry.path());
    std::sort(presets_.begin(),presets_.end()); std::sort(models_.begin(),models_.end());
    selection_=0;
}
int RuntimeUi::native_effect() {
    for(std::size_t i=0;i<draft.pipeline.size();++i){
        const auto& effect=draft.pipeline[i];if(effect.model.empty())continue;
        const auto path=root_/"share/neuroshade"/effect.model;
        const auto key=path.string();auto found=native_models_.find(key);
        if(found==native_models_.end()){
            const auto loaded=neural::load_model_package(path);
            found=native_models_.emplace(key,loaded.valid()&&loaded.package.manifest.runtime=="dlssnr_hip").first;
        }
        if(found->second)return static_cast<int>(i);
    }
    return -1;
}
void RuntimeUi::tick() {
    auto now=Clock::now();
    if (previous_!=Clock::time_point{}) {
        double ms=std::chrono::duration<double,std::milli>(now-previous_).count();
        if (ms>0 && ms<2000) {
            frame_ms_=frame_ms_==0?ms:frame_ms_*0.95+ms*0.05;
            fps_=1000/frame_ms_;
            history_.push_back(ms); if (history_.size()>180) history_.erase(history_.begin());
        }
    }
    previous_=now; ++frames_;
}
void RuntimeUi::sample_gpus() {
    if (gpus_.empty()) {
        std::error_code ec;
        for (const auto& entry:std::filesystem::directory_iterator("/sys/class/drm",ec)) {
            auto name=entry.path().filename().string();
            if (!name.starts_with("card") || name.find('-')!=std::string::npos) continue;
            auto device=entry.path()/"device";
            if (!std::filesystem::exists(device/"vendor",ec)) continue;
            std::string label=read_line(device/"product_name");
            if (label.empty()) label=pci_name(device);
            if (label.empty()) label=read_line(device/"vendor")+":"+read_line(device/"device");
            gpus_.push_back({device,name+"  "+label});
        }
        std::sort(gpus_.begin(),gpus_.end(),[](auto& a,auto& b){return a.name<b.name;});
    }
    for (auto& gpu:gpus_) {
        gpu.busy=read_number(gpu.path/"gpu_busy_percent");
        gpu.used=read_number(gpu.path/"mem_info_vram_used",1048576);
        gpu.total=read_number(gpu.path/"mem_info_vram_total",1048576);
        std::error_code ec;
        for (const auto& hw:std::filesystem::directory_iterator(gpu.path/"hwmon",ec)) {
            gpu.watts=read_number(hw.path()/"power1_average",1e6);
            if (gpu.watts<0) gpu.watts=read_number(hw.path()/"power1_input",1e6);
            gpu.temp=read_number(hw.path()/"temp1_input",1000);
        }
    }
}
void RuntimeUi::note(const std::string& value) {
    status_=value; messages_.push_back(value); if (messages_.size()>50) messages_.erase(messages_.begin()); refresh_={};
}
void RuntimeUi::applied(bool ok,const std::string& error) {
    apply_requested=false;
    if (ok) {dirty_=false; note("Aplicado ao jogo. Use Salvar para manter na proxima sessao.");}
    else note("Nao aplicado: "+error);
}
void RuntimeUi::action(int id) {
    if (id>=100 && id<108) {tab_=id-100;focus_=0;page_=0;return;}
    if (id==1) {apply_requested=true;return;}
    if (id==2 || id==3) {
        std::string error;
        auto target=path_;
        if (id==3) {
            const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            target=presets_dir_/("preset-"+std::to_string(stamp)+".json");
        }
        if (profile::save(draft,target,error)) {
            note("Salvo: "+target.filename().string()); scan();
        } else note("Erro ao salvar: "+error);
        return;
    }
    if (id==4) {draft=defaults_;dirty_=true;note("Valores iniciais restaurados. Clique Aplicar.");return;}
    if (id==5) {scan();note("Lista atualizada.");return;}
    if (id==6 && !presets_.empty()) {
        const auto selected=std::clamp(selection_,0,static_cast<int>(presets_.size())-1);
        const auto loaded=profile::load(presets_[selected]);
        if (loaded.valid()) {draft=loaded.profile;dirty_=true;note("Preset carregado para edicao. Clique Aplicar.");}
        else note("Preset invalido.");
        return;
    }
    if (id==7 || id==8) {
        if(id==7) show_fps_=!show_fps_; else show_profiler_=!show_profiler_;
        std::ofstream settings(path_.string()+".overlay");settings<<show_fps_<<' '<<show_profiler_<<'\n';
        if(!settings) note("Falha ao salvar preferencias do painel.");
        return;
    }
    if(id==13 && !models_.empty()) {
        const int index=std::clamp(selection_,0,static_cast<int>(models_.size())-1);
        const auto loaded=neural::load_model_package(models_[index]);
        if(!loaded.valid()) {note("Modelo invalido: "+loaded.errors.front());return;}
        if(loaded.package.manifest.history!=0) {note("Este motor recebe modelos espaciais; modelo temporal requer motion/depth.");return;}
        std::erase_if(draft.pipeline,[](const auto& e){return !e.model.empty();});
        profile::Effect effect;effect.plugin="org.neuroshade.reconstruction";effect.model="models/"+models_[index].filename().string();effect.strength=0.65f;
        draft.pipeline.insert(draft.pipeline.begin(),std::move(effect));dirty_=true;
        note("Modelo selecionado. Aplicar aquece o runtime antes de reconstruir.");return;
    }
    if(id==14) {
        std::erase_if(draft.pipeline,[](const auto& e){return !e.model.empty();});dirty_=true;
        note("Reconstrucao removida do rascunho. Clique Aplicar.");return;
    }
    if (id==12 && !models_.empty()) {
        const int index=std::clamp(selection_,0,static_cast<int>(models_.size())-1);
        const auto model=neural::load_model_package(models_[index]);
        if(!model.valid()) note("Modelo invalido: "+(model.errors.empty()?std::string("erro desconhecido"):model.errors.front()));
        else {
            const auto& m=model.package.manifest;
            note(m.name+" v"+m.version+" / "+m.runtime+" / escala "+number(m.scale_x)+"x / entradas "+std::to_string(m.inputs.size()));
        }
        return;
    }
    if (id==9) {history_.clear();frames_=0;note("Historico de quadros limpo.");return;}
    if (id==10) {page_=std::max(0,page_-1);return;}
    if (id==11) {++page_;return;}
    if (id>=1000 && id<2000) {selection_=id-1000;return;}
    if(id>=30000&&id<30100){
        const int index=native_effect();if(index<0)return;
        auto& effect=draft.pipeline[static_cast<std::size_t>(index)];
        if(id==30000)effect.enabled=!effect.enabled;
        if(id==30001)effect.strength=std::max(0.f,effect.strength-.05f);
        if(id==30002)effect.strength=std::min(1.f,effect.strength+.05f);
        if(id==30003){effect.strength=1.f;effect.nr_controls={0.f,1.f,1.f};effect.nr_auto_mask=true;effect.nr_style=effect.nr_preset=0;effect.nr_intensity=1.f;}
        if(id==30005)effect.nr_intensity=std::max(0.f,effect.nr_intensity-.05f);
        if(id==30006)effect.nr_intensity=std::min(2.f,effect.nr_intensity+.05f);
        if(id==30007)effect.nr_style=(effect.nr_style+1)%3;
        if(id==30008)effect.nr_preset=(effect.nr_preset+1)%4;
        if(id==30004)effect.nr_auto_mask=!effect.nr_auto_mask;
        if(id>=30010&&id<30016){
            auto& value=effect.nr_controls[static_cast<std::size_t>((id-30010)/2)];
            value=std::clamp(value+((id-30010)%2?.05f:-.05f),id>=30014?-1.f:0.f,2.f);
        }
        dirty_=true;return;
    }
    if (id>=2000) {
        const auto index=static_cast<std::size_t>((id-2000)/10); int op=(id-2000)%10;
        if (index>=draft.pipeline.size()) return;
        auto& effect=draft.pipeline[index];
        if (op==0) effect.enabled=!effect.enabled;
        if (op==1) effect.strength=std::max(0.f,effect.strength-0.05f);
        if (op==2) effect.strength=std::min(1.f,effect.strength+0.05f);
        if (op==3 && index>0) std::swap(draft.pipeline[index],draft.pipeline[index-1]);
        if (op==4 && index+1<draft.pipeline.size()) std::swap(draft.pipeline[index],draft.pipeline[index+1]);
        dirty_=true;
    }
}
void RuntimeUi::rect(int x,int y,int w,int h,std::uint32_t color) {
    for(int row=std::max(0,y);row<std::min(static_cast<int>(height_),y+h);++row)
        for(int col=std::max(0,x);col<std::min(static_cast<int>(width_),x+w);++col)
            pixels_[static_cast<std::size_t>(row)*width_+col]=color;
}
void RuntimeUi::text(int x,int y,const std::string& value,std::uint32_t color) {
    for(unsigned char c:value) {
        if(x+10>=static_cast<int>(width_)) break;
        if(c<32 || c>126) c='?';
        for(int row=0;row<20;++row) for(int col=0;col<10;++col)
            if(glyphs[c-32][row] & (1u<<col)) rect(x+col,y+row,1,1,color);
        x+=9;
    }
}
void RuntimeUi::panel(int x,int y,int w,int h,const std::string& title) {
    rect(x,y,w,h,0xf018202b);rect(x,y,w,3,0xffe1ae54);text(x+14,y+12,title,0xffe1ae54);
}
void RuntimeUi::button(int x,int y,int w,const std::string& label,int id) {
    int index=static_cast<int>(buttons_.size());
    buttons_.push_back({x,y,w,29,label,id});
    rect(x,y,w,29,index==focus_?0xff3b506b:0xff29374a);
    if(index==focus_) rect(x,y,3,29,0xffe1ae54);
    text(x+8,y+4,label);
}
void RuntimeUi::draw(unsigned width,unsigned height,const Snapshot& snapshot,
                     const std::vector<UiInput>& events,bool mouse,double gpu_ms,bool gpu_timer) {
    bool changed=false;
    for(const auto& event:events) {
        if(event.click) {
            for(const auto& b:buttons_) if(event.x>=b.x && event.x<b.x+b.w && event.y>=b.y && event.y<b.y+b.h) {
                action(b.action);changed=true;break;
            }
        }
        if(event.key>=0xffbe && event.key<=0xffc5) {action(100+event.key-0xffbe);changed=true;}
        if(event.key==0xff52 || event.key==0xff51) {focus_=std::max(0,focus_-1);changed=true;}
        if(event.key==0xff54 || event.key==0xff53) {focus_=std::min(static_cast<int>(buttons_.size())-1,focus_+1);changed=true;}
        if((event.key==0xff0d || event.key==0x20) && focus_>=0 && focus_<static_cast<int>(buttons_.size())) {action(buttons_[focus_].action);changed=true;}
        if(event.key==0xff55) {action(10);changed=true;}
        if(event.key==0xff56) {action(11);changed=true;}
    }
    auto now=Clock::now();
    if(!changed && width==width_ && height==height_ && now-refresh_<std::chrono::milliseconds(100)) return;
    refresh_=now;
    if(now-sensors_>std::chrono::seconds(1)) {sample_gpus();sensors_=now;}
    width_=width;height_=height;pixels_.assign(static_cast<std::size_t>(width)*height,0);buttons_.clear();
    const int pw=std::min(870,static_cast<int>(width)-32);
    const int ph=std::min(590,static_cast<int>(height)-270);
    panel(16,16,pw,ph,"NEUROSHADE  /  "+snapshot.game);
    text(32,51,"Home: fechar  |  F1-F8: paginas  |  Setas + Enter: controles",0xff9aabc1);
    text(32,74,mouse?"Mouse capturado pelo painel":"Use o teclado: mouse indisponivel nesta janela",0xff9aabc1);
    const char* tabs[]={"Efeitos","Presets","Modelos","GPUs","Ajustes","Recursos","Logs","DLSSNR"};
    for(int i=0;i<8;++i) button(32+i*103,105,99,std::string(tabs[i]),100+i);
    rect(32+tab_*103,135,99,3,0xffe1ae54);
    int x=32,y=152;
    int available=std::max(1,(ph-270)/42);
    if(tab_==0) {
        text(x,y,"PIPELINE   "+std::string(dirty_?"* alteracoes pendentes":"aplicado"),0xffe1ae54);y+=28;
        text(x,y,"Efeito                  Ativo      Intensidade          Ordem",0xff9aabc1);y+=26;
        page_=std::min(page_,std::max(0,(static_cast<int>(draft.pipeline.size())-1)/available));
        for(int i=page_*available;i<std::min(static_cast<int>(draft.pipeline.size()),(page_+1)*available);++i) {
            auto& e=draft.pipeline[i];text(x,y+4,(e.model.empty()?short_id(e.plugin):std::filesystem::path(e.model).stem().string()).substr(0,22));
            button(x+230,y,75,e.enabled?"Ligado":"Off",2000+i*10);
            button(x+320,y,34,"-",2001+i*10);text(x+365,y+4,number(e.strength,2));button(x+430,y,34,"+",2002+i*10);
            button(x+490,y,72,"Subir",2003+i*10);button(x+572,y,78,"Descer",2004+i*10);y+=42;
        }
        button(x,ph-102,110,"Aplicar",1);button(x+122,ph-102,110,"Salvar",2);
        button(x+244,ph-102,164,"Restaurar",4);
    } else if(tab_==1) {
        text(x,y,"PRESETS  /  perfil de inicio: "+path_.filename().string(),0xffe1ae54);y+=30;
        page_=std::min(page_,std::max(0,(static_cast<int>(presets_.size())-1)/available));
        for(int i=page_*available;i<std::min(static_cast<int>(presets_.size()),(page_+1)*available);++i) {
            button(x,y,650,std::string(selection_==i?"> ":"  ")+presets_[i].filename().string(),1000+i);y+=38;
        }
        button(x,ph-102,108,"Carregar",6);button(x+120,ph-102,100,"Aplicar",1);
        button(x+232,ph-102,190,"Salvar novo preset",3);button(x+434,ph-102,140,"Atualizar",5);
    } else if(tab_==2) {
        text(x,y,"MODELOS INSTALADOS",0xffe1ae54);y+=29;
        text(x,y,"Motores: HIP nativo / PyTorch / ONNX Runtime.");y+=24;
        text(x,y,"Selecione um modelo -> Usar modelo -> Aplicar.",0xff9aabc1);y+=33;
        if(models_.empty()) text(x,y,"Nenhum pacote .nsmodel instalado.");
        page_=std::min(page_,std::max(0,(static_cast<int>(models_.size())-1)/available));
        for(int i=page_*available;i<std::min(static_cast<int>(models_.size()),(page_+1)*available);++i) {
            button(x,y,650,std::string(selection_==i?"> ":"  ")+models_[i].filename().string().substr(0,65),1000+i);y+=38;
        }
        if(!runtime_.empty()) text(x,ph-132,runtime_.substr(0,87),0xff9aabc1);
        button(x,ph-102,124,"Usar modelo",13);button(x+134,ph-102,96,"Aplicar",1);
        button(x+240,ph-102,150,"Sem modelo",14);button(x+400,ph-102,100,"Validar",12);
        button(x+510,ph-102,120,"Atualizar",5);
    } else if(tab_==3) {
        text(x,y,"GPU DO JOGO: "+snapshot.gpu.substr(0,65),0xffe1ae54);y+=32;
        for(const auto& gpu:gpus_) {
            if(y>ph-145) break;
            text(x,y,gpu.name.substr(0,87));y+=22;
            text(x,y,"Uso "+number(gpu.busy,0)+"%   VRAM "+number(gpu.used,0)+" / "+number(gpu.total,0)+" MiB");y+=22;
            text(x,y,"Potencia "+number(gpu.watts)+" W   Temperatura "+number(gpu.temp)+" C",0xff9aabc1);y+=32;
        }
        text(x,ph-99,"Sensores do sistema, todas as GPUs. N/D = nao exposto.",0xff9aabc1);
    } else if(tab_==4) {
        text(x,y,"VISUALIZACAO E DIAGNOSTICO",0xffe1ae54);y+=38;
        button(x,y,360,std::string(show_fps_?"[x] ":"[ ] ")+"FPS / quadros no canto direito",7);y+=44;
        button(x,y,450,std::string(show_profiler_?"[x] ":"[ ] ")+"Perfilador no canto inferior esquerdo",8);y+=44;
        button(x,y,260,"Limpar historico de quadros",9);y+=48;
        text(x,y,"FPS mede apresentacoes do jogo; nao frames gerados.",0xff9aabc1);y+=26;
        text(x,y,"GPU ms mede o NeuroShade, incluindo composicao do painel.",0xff9aabc1);
    } else if(tab_==5) {
        text(x,y,"RECURSOS DETECTADOS",0xffe1ae54);y+=32;
        text(x,y,"Candidatos de analise; deteccao nao confirma binding.",0xff9aabc1);y+=30;
        for(const auto& resource:snapshot.resources) {
            text(x,y,resource.semantic+"  "+std::to_string(resource.width)+"x"+std::to_string(resource.height)+"  confianca "+number(resource.confidence*100,0)+"%");y+=30;
        }
        if(snapshot.resources.empty()) text(x,y,"Aguardando recursos do jogo.");
    } else if(tab_==7){
        text(x,y,"DLSSNR  /  "+std::string(dirty_?"* alteracoes pendentes":"aplicado"),0xffe1ae54);y+=32;
        const int index=native_effect();
        if(index<0){
            text(x,y,"Nenhum modelo DLSSNR no perfil.");y+=30;
            text(x,y,"Selecione o pacote na pagina Modelos (F3).",0xff9aabc1);
        }else{
            auto& e=draft.pipeline[static_cast<std::size_t>(index)];
            button(x,y,160,e.enabled?"NR: Ligado":"NR: Desligado",30000);y+=36;
            text(x,y,"NR Preset");button(x+225,y-4,160,e.nr_preset?"Preset #"+std::to_string(e.nr_preset):"Default",30008);
            text(x+405,y,e.nr_preset>1?"Efetivo: #1 (fallback)":"Efetivo: #1",0xff9aabc1);y+=30;
            const char* styles[]={"Default","Natural","Cinematic"};
            text(x,y,"NR Style");button(x+225,y-4,160,styles[std::min(e.nr_style,2u)],30007);y+=30;
            text(x,y+4,"NR Intensity");button(x+225,y,34,"-",30005);
            text(x+278,y+4,number(e.nr_intensity,2));button(x+352,y,34,"+",30006);
            text(x+430,y+4,"Mistura");button(x+530,y,34,"-",30001);
            text(x+580,y+4,number(e.strength*100,0)+"%");button(x+665,y,34,"+",30002);y+=36;
            text(x,y,"Automatic Mask");button(x+225,y-4,160,e.nr_auto_mask?"Ligado":"Desligado",30004);y+=30;
            const char* labels[]={"Local Tone","Local Structure","Skin Structure"};
            for(int control=0;control<3;++control){
                text(x,y+4,labels[control]);button(x+225,y,34,"-",30010+control*2);
                text(x+278,y+4,control==2&&e.nr_controls[control]<0?"Herdar":number(e.nr_controls[control],2));button(x+352,y,34,"+",30011+control*2);y+=32;
            }
            text(x,y+8,"Intensity: 0=original, 1=normal, 2=ampliado. Pele negativa: herdar.",0xff9aabc1);
            button(x,ph-102,110,"Aplicar",1);button(x+122,ph-102,110,"Salvar",2);
            button(x+244,ph-102,160,"Restaurar NR",30003);
        }
    } else {
        text(x,y,"EVENTOS DA INTERFACE",0xffe1ae54);y+=32;
        int first=std::max(0,static_cast<int>(messages_.size())-available);
        for(int i=first;i<static_cast<int>(messages_.size());++i) {text(x,y,messages_[i].substr(0,87));y+=28;}
        if(messages_.empty()) text(x,y,"Nenhuma alteracao nesta sessao.");
    }
    if(tab_<=2) {button(pw-200,ph-102,90,"Anterior",10);button(pw-100,ph-102,90,"Proxima",11);}
    text(32,ph-46,status_.substr(0,90),0xffe1ae54);
    text(32,ph-22,"Pagina: "+std::string(tabs[tab_])+"  |  ajustes de efeitos: Aplicar -> Salvar",0xff9aabc1);
    if(show_fps_) {
        const int fx=std::max(16,static_cast<int>(width)-320);
        panel(fx,16,304,100,"QUADROS / APRESENTACAO");
        text(fx+14,49,number(fps_,1)+" FPS   "+number(frame_ms_,2)+" ms");
        text(fx+14,77,"Quadros: "+std::to_string(frames_),0xff9aabc1);
    }
    if(show_profiler_) {
        const int py=static_cast<int>(height)-218;
        panel(16,py,530,202,"PERFILADOR  /  NEUROSHADE");
        text(30,py+39,"GPU: "+(gpu_timer?number(gpu_ms,3)+" ms":"N/D")+"   Quadro: "+number(frame_ms_,2)+" ms");
        const double maximum=history_.empty()?0:*std::max_element(history_.begin(),history_.end());
        text(30,py+66,"Inferencia: "+number(inference_ms_,2)+" ms  Pico: "+number(maximum,2)+" ms",0xff9aabc1);
        rect(30,py+99,498,72,0xff101720);
        for(std::size_t i=0;i<history_.size();++i) {
            const int h=std::clamp(static_cast<int>(history_[i]/std::max(33.3,maximum)*66),1,66);
            rect(32+static_cast<int>(i)*2,py+169-h,2,h,0xff63cdb5);
        }
        text(30,py+174,"Historico de intervalos entre apresentacoes",0xff9aabc1);
    }
}
}
