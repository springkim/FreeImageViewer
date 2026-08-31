//
// qoi_decoder.cpp
// QOI(Quite OK Image) 디코더. 스펙: https://qoiformat.org/qoi-specification.pdf
// 포맷이 단순해 외부 라이브러리 없이 직접 구현한다.
//
#include "qoi_decoder.h"

#include <cstdio>
#include <cstring>

namespace {
    // 청크 태그
    constexpr uint8_t QOI_OP_INDEX = 0x00;   // 상위 2비트 00
    constexpr uint8_t QOI_OP_DIFF  = 0x40;   // 상위 2비트 01
    constexpr uint8_t QOI_OP_LUMA  = 0x80;   // 상위 2비트 10
    constexpr uint8_t QOI_OP_RUN   = 0xC0;   // 상위 2비트 11
    constexpr uint8_t QOI_OP_RGB   = 0xFE;
    constexpr uint8_t QOI_OP_RGBA  = 0xFF;

    constexpr size_t QOI_HEADER_SIZE = 14;   // "qoif" + w(4) + h(4) + channels + colorspace
    constexpr size_t QOI_END_SIZE    = 8;    // 종료 마커: 0x00 x7 + 0x01

    // 최대 픽셀 수 제한(악의적/손상 파일로 인한 과도한 할당 방지)
    constexpr uint64_t kMaxPixels = 400ULL * 1000 * 1000;   // 4억 픽셀(≈1.6GB RGBA)

    struct Rgba {
        uint8_t r = 0, g = 0, b = 0, a = 255;
    };

    inline int color_hash(const Rgba& c) {
        return (c.r * 3 + c.g * 5 + c.b * 7 + c.a * 11) % 64;
    }

    uint32_t read_be32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    bool read_file(const std::string& path, std::vector<uint8_t>& out) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(f);
            return false;
        }
        out.resize((size_t)size);
        const size_t got = std::fread(out.data(), 1, (size_t)size, f);
        std::fclose(f);
        return got == (size_t)size;
    }
} // namespace

DecodedImage decode_qoi(const std::string& path) {
    DecodedImage img;

    std::vector<uint8_t> data;
    if (!read_file(path, data)) {
        img.error = "QOI 파일을 읽을 수 없습니다: " + path;
        return img;
    }
    if (data.size() < QOI_HEADER_SIZE + QOI_END_SIZE ||
        std::memcmp(data.data(), "qoif", 4) != 0) {
        img.error = "올바른 QOI 파일이 아닙니다: " + path;
        return img;
    }

    const uint32_t w = read_be32(data.data() + 4);
    const uint32_t h = read_be32(data.data() + 8);
    const uint8_t channels = data[12];     // 3(RGB) 또는 4(RGBA) — 참고용, 디코딩엔 영향 없음
    const uint8_t colorspace = data[13];   // 0 또는 1 — 참고용
    if (w == 0 || h == 0 || (channels != 3 && channels != 4) || colorspace > 1) {
        img.error = "QOI 헤더가 손상되었습니다: " + path;
        return img;
    }
    if ((uint64_t)w * h > kMaxPixels) {
        img.error = "QOI 이미지가 너무 큽니다: " + path;
        return img;
    }

    const size_t pixelCount = (size_t)w * h;
    std::vector<uint8_t> out(pixelCount * 4);

    Rgba index[64];                 // 직전에 본 색 64개 해시 테이블
    Rgba px;                        // 현재 픽셀(초기값 0,0,0,255)
    const uint8_t* p = data.data() + QOI_HEADER_SIZE;
    const uint8_t* end = data.data() + data.size() - QOI_END_SIZE;

    size_t written = 0;             // 기록한 픽셀 수
    int run = 0;
    while (written < pixelCount) {
        if (run > 0) {
            --run;
        } else if (p < end) {
            const uint8_t b1 = *p++;
            if (b1 == QOI_OP_RGB) {
                if (end - p < 3) { break; }
                px.r = p[0]; px.g = p[1]; px.b = p[2];
                p += 3;
            } else if (b1 == QOI_OP_RGBA) {
                if (end - p < 4) { break; }
                px.r = p[0]; px.g = p[1]; px.b = p[2]; px.a = p[3];
                p += 4;
            } else if ((b1 & 0xC0) == QOI_OP_INDEX) {
                px = index[b1 & 0x3F];
            } else if ((b1 & 0xC0) == QOI_OP_DIFF) {
                px.r += ((b1 >> 4) & 0x03) - 2;
                px.g += ((b1 >> 2) & 0x03) - 2;
                px.b += (b1 & 0x03) - 2;
            } else if ((b1 & 0xC0) == QOI_OP_LUMA) {
                if (end - p < 1) { break; }
                const uint8_t b2 = *p++;
                const int dg = (b1 & 0x3F) - 32;
                px.r += dg - 8 + ((b2 >> 4) & 0x0F);
                px.g += dg;
                px.b += dg - 8 + (b2 & 0x0F);
            } else {   // QOI_OP_RUN
                run = (b1 & 0x3F);   // 이번 픽셀 + run 개 반복(길이 1..62, bias -1)
            }
            index[color_hash(px)] = px;
        } else {
            break;   // 데이터가 픽셀 수보다 먼저 끝남(손상 파일) — 채운 데까지만 표시
        }
        out[written * 4 + 0] = px.r;
        out[written * 4 + 1] = px.g;
        out[written * 4 + 2] = px.b;
        out[written * 4 + 3] = px.a;
        ++written;
    }

    if (written == 0) {
        img.error = "QOI 디코딩에 실패했습니다: " + path;
        return img;
    }

    img.width = (int)w;
    img.height = (int)h;
    img.pixels = std::move(out);
    img.ok = true;
    return img;
}
