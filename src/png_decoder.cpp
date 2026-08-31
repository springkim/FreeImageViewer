//
// png_decoder.cpp
// libspng 로 PNG 를 RGBA8 로 디코딩한다.
//
#include "png_decoder.h"

#include <spng.h>

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

DecodedImage decode_png(const std::string& path) {
    DecodedImage img;

    std::vector<uint8_t> png;
    if (!read_file(path, png, img.error)) {
        return img;
    }

    spng_ctx* ctx = spng_ctx_new(0);
    if (!ctx) {
        img.error = "spng_ctx_new 실패";
        return img;
    }

    // 디코딩할 PNG 버퍼 지정
    int ret = spng_set_png_buffer(ctx, png.data(), png.size());
    if (ret != 0) {
        img.error = std::string("spng_set_png_buffer 실패: ") + spng_strerror(ret);
        spng_ctx_free(ctx);
        return img;
    }

    // 헤더에서 크기 추출
    spng_ihdr ihdr;
    ret = spng_get_ihdr(ctx, &ihdr);
    if (ret != 0) {
        img.error = std::string("PNG 헤더 파싱 실패: ") + spng_strerror(ret);
        spng_ctx_free(ctx);
        return img;
    }

    // RGBA8 출력에 필요한 버퍼 크기 계산
    size_t out_size = 0;
    ret = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &out_size);
    if (ret != 0) {
        img.error = std::string("이미지 크기 계산 실패: ") + spng_strerror(ret);
        spng_ctx_free(ctx);
        return img;
    }

    img.width  = static_cast<int>(ihdr.width);
    img.height = static_cast<int>(ihdr.height);
    img.pixels.resize(out_size);

    // RGBA8 로 디코딩 (SPNG_DECODE_TRNS: tRNS 청크의 투명도 적용)
    ret = spng_decode_image(ctx, img.pixels.data(), out_size,
                            SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (ret != 0) {
        img.error = std::string("PNG 디코딩 실패: ") + spng_strerror(ret);
        spng_ctx_free(ctx);
        return img;
    }

    spng_ctx_free(ctx);
    img.ok = true;
    return img;
}
