//
// stb_decoder.cpp
// stb_image(단일 헤더, public domain)의 공용 구현체.
// ICO 안의 PNG/BMP, PSD, TGA 및 기존 BMP 디코더가 함께 사용한다.
//
#include "decoder/stb_decoder.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#define STBI_ONLY_PSD
#define STBI_ONLY_TGA
#define STBI_NO_STDIO_WRITE
#include <stb/stb_image.h>

#include <climits>
#include <cstring>
#include <limits>

namespace {
    constexpr uint64_t kMaxPixels = 400ULL * 1000 * 1000;

    bool valid_dimensions(int width, int height) {
        return width > 0 && height > 0 &&
               static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <= kMaxPixels &&
               static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <=
                   std::numeric_limits<size_t>::max() / 4;
    }

    DecodedImage copy_pixels(unsigned char* pixels, int width, int height,
                             const std::string& path, const char* format_name) {
        DecodedImage img;
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            img.error = std::string(format_name) + " 디코딩 실패(" +
                        (reason ? reason : "unknown") + "): " + path;
            return img;
        }
        if (!valid_dimensions(width, height)) {
            stbi_image_free(pixels);
            img.error = std::string("유효하지 않거나 너무 큰 ") + format_name +
                        " 이미지 크기입니다: " + path;
            return img;
        }

        img.width = width;
        img.height = height;
        img.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        std::memcpy(img.pixels.data(), pixels, img.pixels.size());
        stbi_image_free(pixels);
        img.ok = true;
        return img;
    }
}

DecodedImage decoder_detail::decode_stb_file(const std::string& path,
                                              const char* format_name) {
    int width = 0, height = 0, components = 0;
    if (!stbi_info(path.c_str(), &width, &height, &components)) {
        DecodedImage img;
        const char* reason = stbi_failure_reason();
        img.error = std::string(format_name) + " 헤더 확인 실패(" +
                    (reason ? reason : "unknown") + "): " + path;
        return img;
    }
    if (!valid_dimensions(width, height)) {
        DecodedImage img;
        img.error = std::string("유효하지 않거나 너무 큰 ") + format_name +
                    " 이미지 크기입니다: " + path;
        return img;
    }

    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &components, 4);
    return copy_pixels(pixels, width, height, path, format_name);
}

DecodedImage decoder_detail::decode_stb_memory(const uint8_t* data, std::size_t size,
                                                const char* format_name) {
    DecodedImage img;
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX)) {
        img.error = std::string(format_name) + " 데이터 크기가 올바르지 않습니다";
        return img;
    }

    int width = 0, height = 0, components = 0;
    const int data_size = static_cast<int>(size);
    if (!stbi_info_from_memory(data, data_size, &width, &height, &components)) {
        const char* reason = stbi_failure_reason();
        img.error = std::string(format_name) + " 헤더 확인 실패(" +
                    (reason ? reason : "unknown") + ")";
        return img;
    }
    if (!valid_dimensions(width, height)) {
        img.error = std::string("유효하지 않거나 너무 큰 ") + format_name +
                    " 이미지 크기입니다";
        return img;
    }

    unsigned char* pixels = stbi_load_from_memory(data, data_size, &width, &height,
                                                   &components, 4);
    return copy_pixels(pixels, width, height, "메모리", format_name);
}

DecodedImage decode_stb(const std::string& path) {
    return decoder_detail::decode_stb_file(path, "BMP");
}
