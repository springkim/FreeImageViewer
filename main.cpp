#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>

#include <decoder/image_decoder.h>
#include <ttime.h>
#include <core/performance.h>
#include <core/cpu.h>
#include <core/image_list.h>
#include <iostream>

void cpu_info() {
    const auto cpu = core::get_cpu_info();

    std::cout
            << "CPU     : "
            << (cpu.model.empty()
                    ? "Unknown"
                    : cpu.model)
            << '\n';

    std::cout << "Cores   : ";

    if (cpu.cores)
        std::cout << *cpu.cores;
    else
        std::cout << "Unknown";

    std::cout << '\n';

    std::cout << "Threads : ";

    if (cpu.threads)
        std::cout << *cpu.threads;
    else
        std::cout << "Unknown";

    std::cout << '\n';

    if (cpu.p_cores)
        std::cout
                << "P-Cores : "
                << *cpu.p_cores
                << '\n';

    if (cpu.e_cores)
        std::cout
                << "E-Cores : "
                << *cpu.e_cores
                << '\n';
}

int main() {
    // {
    //     core::use_pcore();
    //     auto t_beg = ttime();
    //     //auto files = core::get_image_file_list("/Users/spring/Pictures/ImageDataset/imagenet-1k/image/n02687172_47460_n02687172.JPEG");
    //     auto files = core::get_image_file_list("/Users/spring/Pictures/ImageDataset/supervisely_person_clean_2667_img/images/ds1_people-mother-family-father.png");
    //     auto t_end = ttime();
    //     std::cout << t_end - t_beg << std::endl;
    //     std::cout << files.size() << std::endl;
    //     return 0;
    // }


    core::use_pcore();

    cpu_info();
    std::ifstream fin;
    fin.open("list.txt");
    //fin.open("/Users/spring/PycharmProjects/ImageDatasetProject/image_list.txt");
    if (!fin.is_open()) {
        return -1;
    }
    std::vector<std::string> image_paths;
    std::string line;
    while (std::getline(fin, line)) {
        //std::cout << line << '\n';
        image_paths.push_back(line);
    }
    auto t_beg_total = ttime();
    for (auto &image_path: image_paths) {
        auto t_beg = ttime();
        auto img = decode_image(image_path, true);
        auto t_end = ttime();
        std::cout << image_path << std::endl;
        std::cout << '\t' << img.width << " x " << img.height << "(" << t_end - t_beg << ")" << std::endl;
    }
    auto t_end_total = ttime();

    std::cout << t_end_total - t_beg_total << std::endl;

    return 0;
}
