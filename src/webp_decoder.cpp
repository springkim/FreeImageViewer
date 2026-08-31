//
// webp_decoder.cpp
// libwebp 로 WebP 를 RGBA 로 디코딩한다.
//
#include "webp_decoder.h"

#include <webp/decode.h>

#include <cstdio>

namespace {
    // 파일 전체를 메모리로 읽어들인다.
    bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            error = "파일을 열 수 없습니다: " + path;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            error = "빈 파일이거나 크기를 읽을 수 없습니다: " + path;
            std::fclose(f);
            return false;
        }
        out.resize(static_cast<size_t>(size));
        const size_t got = std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        if (got != out.size()) {
            error = "파일을 끝까지 읽지 못했습니다: " + path;
            return false;
        }
        return true;
    }
} // namespace

DecodedImage decode_webp(const std::string& path) {
    DecodedImage img;

    std::vector<uint8_t> webp;
    if (!read_file(path, webp, img.error)) {
        return img;
    }

    int width = 0;
    int height = 0;

    // RGBA 로 디코딩 (반환 버퍼는 WebPFree 로 해제해야 함)
    uint8_t* rgba = WebPDecodeRGBA(webp.data(), webp.size(), &width, &height);
    if (!rgba) {
        img.error = "WebP 디코딩 실패(손상되었거나 지원하지 않는 WebP)";
        return img;
    }
    if (width <= 0 || height <= 0) {
        img.error = "유효하지 않은 이미지 크기입니다";
        WebPFree(rgba);
        return img;
    }

    img.width  = width;
    img.height = height;
    img.pixels.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);

    WebPFree(rgba);
    img.ok = true;
    return img;
}
