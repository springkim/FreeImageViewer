//
// Created by 김봄 on 2026. 9. 1..
//

#ifndef HG_AB337F1320D4DBB6FD5A6DBD684F9D9C_H
#define HG_AB337F1320D4DBB6FD5A6DBD684F9D9C_H
#include<decoder/image_decoder.h>
#include<core/image_list.h>
#include<core/performance.h>
namespace core {
    DecodedImage image_open(const std::string &image_path) {

        use_pcore();

        auto img = decode_image(image_path);
        auto img_list = get_image_file_list(image_path);

        // 위의
    }
}
#endif //HG_AB337F1320D4DBB6FD5A6DBD684F9D9C_H
