//
// pnm_decoder.cpp
// PNM 계열(PBM/PGM/PPM) 디코더. ASCII(P1/P2/P3)와 바이너리(P4/P5/P6) 모두 지원한다.
// maxval 65535 까지(16비트 샘플, 빅엔디안) 8비트로 스케일링해 디코딩한다.
//
#include "pnm_decoder.h"

#include <cstdio>
#include <cstring>

namespace {
    // 최대 픽셀 수 제한(손상 파일로 인한 과도한 할당 방지)
    constexpr uint64_t kMaxPixels = 400ULL * 1000 * 1000;

    struct Reader {
        const uint8_t* p;
        const uint8_t* end;

        bool eof() const { return p >= end; }

        // 공백/개행과 '#' 주석(줄 끝까지)을 건너뛴다.
        void skip_space_comments() {
            while (p < end) {
                if (*p == '#') {
                    while (p < end && *p != '\n') { ++p; }
                } else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                           *p == '\v' || *p == '\f') {
                    ++p;
                } else {
                    break;
                }
            }
        }

        // 10진 정수 토큰을 읽는다. 실패 시 false.
        bool read_uint(uint32_t& out) {
            skip_space_comments();
            if (eof() || *p < '0' || *p > '9') {
                return false;
            }
            uint64_t v = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                if (v > 0xFFFFFFFFULL) {
                    return false;
                }
                ++p;
            }
            out = (uint32_t)v;
            return true;
        }

        // P1 용: 공백/주석을 건너뛰고 '0' 또는 '1' 한 글자를 읽는다(붙여쓰기 허용).
        bool read_bit(uint32_t& out) {
            skip_space_comments();
            if (eof() || (*p != '0' && *p != '1')) {
                return false;
            }
            out = (uint32_t)(*p - '0');
            ++p;
            return true;
        }
    };

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

    inline uint8_t scale_to_8bit(uint32_t v, uint32_t maxval) {
        if (v > maxval) {
            v = maxval;
        }
        return (uint8_t)((v * 255 + maxval / 2) / maxval);
    }
} // namespace

DecodedImage decode_pnm(const std::string& path) {
    DecodedImage img;

    std::vector<uint8_t> data;
    if (!read_file(path, data)) {
        img.error = "PNM 파일을 읽을 수 없습니다: " + path;
        return img;
    }
    if (data.size() < 2 || data[0] != 'P' || data[1] < '1' || data[1] > '6') {
        img.error = "올바른 PNM 파일이 아닙니다: " + path;
        return img;
    }
    const int type = data[1] - '0';          // 1..6
    const bool binary = (type >= 4);
    const int channels = (type == 3 || type == 6) ? 3 : 1;   // PPM=3, PBM/PGM=1
    const bool bitmap = (type == 1 || type == 4);            // PBM(1=검정)

    Reader r{data.data() + 2, data.data() + data.size()};

    uint32_t w = 0, h = 0, maxval = 1;
    if (!r.read_uint(w) || !r.read_uint(h)) {
        img.error = "PNM 헤더(크기)를 읽지 못했습니다: " + path;
        return img;
    }
    if (!bitmap) {
        if (!r.read_uint(maxval) || maxval == 0 || maxval > 65535) {
            img.error = "PNM 헤더(maxval)가 잘못되었습니다: " + path;
            return img;
        }
    }
    if (w == 0 || h == 0 || (uint64_t)w * h > kMaxPixels) {
        img.error = "PNM 크기가 잘못되었거나 너무 큽니다: " + path;
        return img;
    }

    // 바이너리 형식은 헤더 마지막 정수 뒤 공백 문자 정확히 1개 다음부터 데이터.
    if (binary) {
        if (r.eof()) {
            img.error = "PNM 픽셀 데이터가 없습니다: " + path;
            return img;
        }
        ++r.p;   // 단일 공백(개행) 소비
    }

    const size_t pixelCount = (size_t)w * h;
    std::vector<uint8_t> out(pixelCount * 4);

    auto put = [&](size_t i, uint8_t rr, uint8_t gg, uint8_t bb) {
        out[i * 4 + 0] = rr;
        out[i * 4 + 1] = gg;
        out[i * 4 + 2] = bb;
        out[i * 4 + 3] = 255;
    };

    bool ok = true;
    if (type == 4) {
        // 바이너리 PBM: 행마다 바이트 경계 패딩, MSB 우선, 1=검정
        const size_t rowBytes = ((size_t)w + 7) / 8;
        if ((size_t)(r.end - r.p) < rowBytes * h) {
            ok = false;
        } else {
            for (uint32_t y = 0; y < h && ok; y++) {
                const uint8_t* row = r.p + (size_t)y * rowBytes;
                for (uint32_t x = 0; x < w; x++) {
                    const int bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
                    const uint8_t v = bit ? 0 : 255;
                    put((size_t)y * w + x, v, v, v);
                }
            }
        }
    } else if (binary) {
        // P5/P6: maxval<256 → 1바이트, 그 외 2바이트 빅엔디안
        const int bytesPerSample = (maxval < 256) ? 1 : 2;
        const size_t need = pixelCount * channels * bytesPerSample;
        if ((size_t)(r.end - r.p) < need) {
            ok = false;
        } else {
            const uint8_t* s = r.p;
            for (size_t i = 0; i < pixelCount; i++) {
                uint8_t v[3];
                for (int c = 0; c < channels; c++) {
                    uint32_t raw = (bytesPerSample == 1)
                                       ? *s
                                       : ((uint32_t)s[0] << 8) | s[1];
                    s += bytesPerSample;
                    v[c] = scale_to_8bit(raw, maxval);
                }
                if (channels == 1) {
                    put(i, v[0], v[0], v[0]);
                } else {
                    put(i, v[0], v[1], v[2]);
                }
            }
        }
    } else {
        // P1/P2/P3(ASCII)
        for (size_t i = 0; i < pixelCount && ok; i++) {
            uint32_t v[3] = {0, 0, 0};
            for (int c = 0; c < channels && ok; c++) {
                ok = bitmap ? r.read_bit(v[c]) : r.read_uint(v[c]);
            }
            if (!ok) {
                break;
            }
            if (bitmap) {
                const uint8_t g = v[0] ? 0 : 255;   // 1=검정
                put(i, g, g, g);
            } else if (channels == 1) {
                const uint8_t g = scale_to_8bit(v[0], maxval);
                put(i, g, g, g);
            } else {
                put(i, scale_to_8bit(v[0], maxval), scale_to_8bit(v[1], maxval),
                    scale_to_8bit(v[2], maxval));
            }
        }
    }

    if (!ok) {
        img.error = "PNM 픽셀 데이터가 부족하거나 손상되었습니다: " + path;
        return img;
    }

    img.width = (int)w;
    img.height = (int)h;
    img.pixels = std::move(out);
    img.ok = true;
    return img;
}
