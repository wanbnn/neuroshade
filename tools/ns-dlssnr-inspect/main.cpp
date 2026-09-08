#include "neural/dlssnr/weight_archive.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
int main(int argc, char** argv) {
    try {
        if ((argc != 3 && argc != 5) || (argc == 5 && std::string_view(argv[3]) != "--extract") || (std::string_view(argv[1]) != "--dll" && std::string_view(argv[1]) != "--weights"))
            throw std::runtime_error("usage: ns-dlssnr-inspect --dll library.dll | --weights WEIGHTS_HT.bin [--extract raw.bin]");
        namespace nr = neuroshade::neural::dlssnr;
        const auto archive = std::string_view(argv[1]) == "--dll" ? nr::load_dll_weights(argv[2]) : nr::WeightArchive::load(argv[2]);
        if (argc == 5) {
            std::ofstream output(argv[4], std::ios::binary);
            const auto& bytes = archive.serialized();
            if (!output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("cannot write extracted weights");
        }
        std::cout << "{\"weight_records\":" << archive.records().size()
                  << ",\"payload_bytes\":" << archive.payload_bytes()
                  << ",\"inference_ready\":false,\"records\":[";
        bool first = true;
        for (const auto& record : archive.records()) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << "{\"name\":\"" << record.name << "\",\"offset\":" << record.offset
                      << ",\"bytes\":" << record.bytes << '}';
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ns-dlssnr-inspect: " << error.what() << '\n';
        return 1;
    }
}
