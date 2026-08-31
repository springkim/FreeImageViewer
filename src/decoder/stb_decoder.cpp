//
// stb_decoder.cpp
// stb_image(단일 헤더, public domain)로 BMP/TGA 를 8비트 RGBA 로 디코딩한다.
// (PNM 계열은 ASCII 형식까지 지원하는 자체 구현 pnm_decoder 가 담당)
//
#include "decoder/stb_decoder.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO_WRITE
#include <stb/stb_image.h>

#include <cstring>

DecodedImage decode_stb(const std::string& path) {
    DecodedImage img;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        img.error = std::string("BMP/TGA 디코딩 실패(") +
                    (reason ? reason : "unknown") + "): " + path;
        return img;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        img.error = "유효하지 않은 BMP/TGA 크기입니다: " + path;
        return img;
    }

    img.width = w;
    img.height = h;
    img.pixels.resize((size_t)w * h * 4);
    std::memcpy(img.pixels.data(), pixels, img.pixels.size());
    stbi_image_free(pixels);

    img.ok = true;
    return img;
}
