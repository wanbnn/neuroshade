#pragma once
#include "overlay/overlay_state.hpp"
#include "overlay/home_key_input.hpp"
#include "profile/profile.hpp"
#include <chrono>
#include <filesystem>
#include <vector>
#include <map>
#include <cstdint>

namespace neuroshade::overlay {
class RuntimeUi {
public:
    RuntimeUi(profile::Profile profile, std::filesystem::path path, std::filesystem::path root);
    void tick();
    void draw(unsigned width, unsigned height, const Snapshot& snapshot,
              const std::vector<UiInput>& events, bool mouse, double gpu_ms, bool gpu_timer);
    const std::vector<std::uint32_t>& pixels() const { return pixels_; }
    bool apply_requested{};
    profile::Profile draft;
    void applied(bool ok, const std::string& error = {});
    void note(const std::string& text);
    void runtime_metrics(std::string runtime,double ms) {runtime_=std::move(runtime);inference_ms_=ms;}
private:
    using Clock=std::chrono::steady_clock;
    struct Button { int x,y,w,h; std::string label; int action; };
    struct Gpu { std::filesystem::path path; std::string name; double busy{-1},used{-1},total{-1},watts{-1},temp{-1}; };
    std::filesystem::path path_,root_,presets_dir_;
    profile::Profile defaults_;
    std::vector<std::filesystem::path> presets_,models_;
    std::vector<Gpu> gpus_;
    std::map<std::string,bool> native_models_;
    int native_effect();
    std::vector<Button> buttons_;
    std::vector<std::uint32_t> pixels_;
    std::vector<double> history_;
    std::vector<std::string> messages_;
    Clock::time_point previous_{}, refresh_{}, sensors_{};
    double frame_ms_{}, fps_{};
    std::uint64_t frames_{};
    unsigned width_{},height_{};
    int tab_{},selection_{},focus_{},page_{};
    bool dirty_{}, show_fps_{true}, show_profiler_{true};
    double inference_ms_{};
    std::string runtime_;
    std::string status_{"Pronto. Home abre/fecha; F1-F8 muda de pagina."};
    void scan();
    void sample_gpus();
    void action(int id);
    void rect(int x,int y,int w,int h,std::uint32_t color);
    void text(int x,int y,const std::string& value,std::uint32_t color=0xffe6edf3);
    void button(int x,int y,int w,const std::string& label,int id);
    void panel(int x,int y,int w,int h,const std::string& title);
};
}
