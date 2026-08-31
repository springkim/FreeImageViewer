#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <image_decoder.h>

int main() {
    std::ifstream fin;
    fin.open("list.txt");
    if (!fin.is_open()) {
        return -1;
    }
    std::vector<std::string> image_paths;
    std::string line;
    while (std::getline(fin, line)) {
        //std::cout << line << '\n';
        image_paths.push_back(line);
    }
    for (auto &image_path: image_paths) {
        auto img = decode_image(image_path);
        std::cout << image_path << std::endl;
        std::cout << '\t' << img.width << " x " << img.height << std::endl;
    }


    return 0;
}
