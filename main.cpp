#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <decoder/image_decoder.h>
#include <ttime.h>

int main() {
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
    auto t_beg = ttime();
    for (auto &image_path: image_paths) {
        auto img = decode_image(image_path);
        std::cout << image_path << std::endl;
        std::cout << '\t' << img.width << " x " << img.height << std::endl;
    }
    auto t_end = ttime();
    std::cout << t_end - t_beg << std::endl;


    return 0;
}
