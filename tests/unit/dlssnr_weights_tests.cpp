#include "neural/dlssnr/weight_archive.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <unistd.h>
namespace nr = neuroshade::neural::dlssnr;
using Bytes = std::vector<std::byte>;
void check(bool value) { if (!value) throw std::runtime_error("assertion failed"); }
void put(Bytes& b, std::size_t at, std::uint64_t value, std::size_t width) {
    if (b.size() < at + width) b.resize(at + width);
    for (std::size_t i = 0; i < width; ++i) b[at + i] = std::byte((value >> (i*8)) & 255);
}
void append(Bytes& b, std::uint64_t value, std::size_t width) { put(b, b.size(), value, width); }
void add_record(Bytes& b, const std::string& name) {
    append(b, name.size(), 8);
    for (char c : name) b.push_back(std::byte(c));
    append(b, 44, 8); append(b, 44, 8); append(b, 4, 8); append(b, 1, 4);
    append(b, 0x7e013c00, 4); // Arbitrary packed payload: preserve, do not float-convert.
    append(b, 0, 4); append(b, 0, 4); append(b, 1, 8); append(b, 2, 4);
    put(b, 0, b.size(), 8);
}
void rejects(const Bytes& bytes) {
    try { const nr::WeightArchive archive(bytes); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("malformed archive accepted");
}
int main() {
    try {
        Bytes valid(8); add_record(valid, "block0.layer0.layer");
        const nr::WeightArchive archive(valid);
        check(archive.records().size() == 1 && archive.payload_bytes() == 4);
        const auto data = archive.payload("block0.layer0.layer");
        check(data[0] == std::byte{0} && data[3] == std::byte{0x7e});
        bool missing = false;
        try { (void)archive.payload("absent"); } catch (const std::runtime_error&) { missing = true; }
        check(missing);
        for (std::size_t n = 0; n < valid.size(); ++n) {
            Bytes truncated(valid.begin(), valid.begin()+n);
            // Both outer-length mismatch and internally truncated framing.
            rejects(truncated);
            if (n >= 8) { put(truncated, 0, n, 8); rejects(truncated); }
        }
        auto duplicate = valid; add_record(duplicate, "block0.layer0.layer"); rejects(duplicate);
        auto oversized = valid; put(oversized, 8, UINT64_MAX, 8); rejects(oversized);
        auto device = valid; put(device, 16+18+8+16, 2, 4); rejects(device);
        auto rank = valid; put(rank, rank.size()-12, 33, 8); rejects(rank);
        auto name = valid; name[16] = std::byte{'/'}; rejects(name);
        // A minimal PE32+ with a named RCDATA resource at a non-hardcoded RVA.
        Bytes pe(0x300 + valid.size());
        put(pe, 0, 0x5a4d, 2); put(pe, 0x3c, 0x80, 4);
        put(pe, 0x80, 0x4550, 4); put(pe, 0x84, 0x8664, 2); put(pe, 0x86, 1, 2);
        put(pe, 0x94, 240, 2); put(pe, 0x98, 0x20b, 2); put(pe, 0x98+108, 16, 4);
        put(pe, 0x98+128, 0x4000, 4); put(pe, 0x98+132, 0x100+valid.size(), 4);
        put(pe, 0x188+12, 0x4000, 4); put(pe, 0x188+16, 0x100+valid.size(), 4); put(pe, 0x188+20, 0x200, 4);
        put(pe, 0x200+14, 1, 2); put(pe, 0x210, 10, 4); put(pe, 0x214, 0x80000020, 4);
        put(pe, 0x220+12, 1, 2); put(pe, 0x230, 0x80000080, 4); put(pe, 0x234, 0x80000040, 4);
        put(pe, 0x240+14, 1, 2); put(pe, 0x250, 1033, 4); put(pe, 0x254, 0x60, 4);
        put(pe, 0x260, 0x4100, 4); put(pe, 0x264, valid.size(), 4);
        put(pe, 0x280, 10, 2);
        const std::string resource = "WEIGHTS_HT";
        for (std::size_t i=0; i<resource.size(); ++i) put(pe,0x282+i*2,resource[i],2);
        std::copy(valid.begin(),valid.end(),pe.begin()+0x300);
        const auto path = std::filesystem::temp_directory_path() / ("ns-dlssnr-test-"+std::to_string(getpid())+".dll");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{path};
        auto write = [&] { std::ofstream f(path,std::ios::binary); f.write(reinterpret_cast<const char*>(pe.data()),pe.size()); };
        write(); check(nr::load_dll_weights(path).payload_bytes() == 4);
        put(pe,0x264,UINT32_MAX,4); write();
        bool bad_pe=false;
        try { (void)nr::load_dll_weights(path); } catch (const std::runtime_error&) { bad_pe=true; }
        check(bad_pe);
        std::cout << "dlssnr_weights=pass\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
