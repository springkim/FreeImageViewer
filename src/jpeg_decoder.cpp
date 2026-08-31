//
// jpeg_decoder.cpp
// libjpeg-turbo 의 최신 turbojpeg(tj3_) API 로 JPEG 를 RGBA 로 디코딩한다.
//
#include "jpeg_decoder.h"

#include <turbojpeg.h>

#include <cstdio>

namespace {
    // 파일 전체를 메모리로 읽어들인다.
    bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
        FILE *f = std::fopen(path.c_str(), "rb");
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

    // 미리보기 임계값: 이보다 작은 JPEG 은 미리보기 이득이 없어 그냥 전체 디코딩한다.
    constexpr long long kPreviewMinPixels = 1024LL * 1024LL; // 약 1메가픽셀

    // 메모리에 올린 JPEG 을 scaleNum/scaleDenom 배율로 디코딩한다(1/1 = 원본).
    DecodedImage decode_jpeg_buffer(const std::vector<uint8_t> &jpeg,
                                    int scaleNum, int scaleDenom) {
        DecodedImage img;

        tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
        if (!handle) {
            img.error = "tj3Init 실패";
            return img;
        }

        // JPEG 헤더에서 원본 크기 추출
        if (tj3DecompressHeader(handle, jpeg.data(), jpeg.size()) != 0) {
            img.error = std::string("JPEG 헤더 파싱 실패: ") + tj3GetErrorStr(handle);
            tj3Destroy(handle);
            return img;
        }

        const int fullW = tj3Get(handle, TJPARAM_JPEGWIDTH);
        const int fullH = tj3Get(handle, TJPARAM_JPEGHEIGHT);
        if (fullW <= 0 || fullH <= 0) {
            img.error = "유효하지 않은 이미지 크기입니다";
            tj3Destroy(handle);
            return img;
        }

        // DCT 스케일링 설정(1/8 등)으로 저해상도를 빠르게 디코딩
        int outW = fullW;
        int outH = fullH;
        if (scaleNum != scaleDenom) {
            tjscalingfactor sf;
            sf.num = scaleNum;
            sf.denom = scaleDenom;
            if (tj3SetScalingFactor(handle, sf) != 0) {
                img.error = std::string("JPEG 스케일 설정 실패: ") + tj3GetErrorStr(handle);
                tj3Destroy(handle);
                return img;
            }
            outW = TJSCALED(fullW, sf);
            outH = TJSCALED(fullH, sf);
        }

        img.width = outW;
        img.height = outH;
        img.fullWidth = fullW;
        img.fullHeight = fullH;
        img.pixels.resize(static_cast<size_t>(outW) * outH * 4); // RGBA

        // RGBA 로 디코딩 (pitch = 0 이면 outW * pixelSize 로 자동 계산)
        if (tj3Decompress8(handle, jpeg.data(), jpeg.size(),
                           img.pixels.data(), /*pitch=*/0, TJPF_RGBA) != 0) {
            img.error = std::string("JPEG 디코딩 실패: ") + tj3GetErrorStr(handle);
            tj3Destroy(handle);
            return img;
        }

        tj3Destroy(handle);
        img.ok = true;
        return img;
    }
} // namespace

DecodedImage decode_jpeg(const std::string &path) {
    DecodedImage img;
    std::vector<uint8_t> jpeg;
    if (!read_file(path, jpeg, img.error)) {
        return img;
    }
    return decode_jpeg_buffer(jpeg, 1, 1);
}

DecodedImage decode_jpeg_preview(const std::string &path) {
    DecodedImage img;
    std::vector<uint8_t> jpeg;
    if (!read_file(path, jpeg, img.error)) {
        return img;
    }

    // 헤더만 빠르게 보고, 큰 이미지에만 미리보기를 제공한다.
    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (!handle) {
        img.error = "tj3Init 실패";
        return img; // ok=false
    }
    if (tj3DecompressHeader(handle, jpeg.data(), jpeg.size()) != 0) {
        tj3Destroy(handle);
        img.error = "JPEG 헤더 파싱 실패";
        return img;
    }
    const long long w = tj3Get(handle, TJPARAM_JPEGWIDTH);
    const long long h = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    tj3Destroy(handle);

    if (w * h < kPreviewMinPixels) {
        img.ok = false; // 작은 이미지 → 미리보기 생략(호출자가 전체 디코딩)
        return img;
    }

    // 1/8 해상도(원본의 1/64 픽셀)로 매우 빠르게 디코딩
    return decode_jpeg_buffer(jpeg, 1, 8);
}
