//
// image_decoder.cpp
// 파일 시그니처(매직 바이트)를 보고 포맷별 디코더로 분기한다.
//
#include "image_decoder.h"

#include "jpeg_decoder.h"
#include "png_decoder.h"
#include "webp_decoder.h"
#include "gif_decoder.h"
#include "tiff_decoder.h"
#include "avif_decoder.h"
#include "qoi_decoder.h"
#include "jxl_decoder.h"
#include "jpeg2000_decoder.h"
#include "stb_decoder.h"
#include "pnm_decoder.h"
#include "exr_decoder.h"
#include "heif_decoder.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {
    // 파일 앞부분 몇 바이트를 읽는다.
    size_t read_magic(const std::string& path, uint8_t* buf, size_t n) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            return 0;
        }
        const size_t got = std::fread(buf, 1, n, f);
        std::fclose(f);
        return got;
    }

    bool is_png(const uint8_t* p, size_t n) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        return n >= 8 && std::memcmp(p, sig, 8) == 0;
    }

    bool is_jpeg(const uint8_t* p, size_t n) {
        return n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF;
    }

    // JPEG 2000: JP2 컨테이너 시그니처 또는 원시 J2K 코드스트림(SOC, SIZ).
    bool is_jpeg2000(const uint8_t* p, size_t n) {
        static const uint8_t jp2[12] = {
            0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ',
            0x0D, 0x0A, 0x87, 0x0A
        };
        if (n >= 12 && std::memcmp(p, jp2, 12) == 0) {
            return true;
        }
        return n >= 4 && p[0] == 0xFF && p[1] == 0x4F &&
               p[2] == 0xFF && p[3] == 0x51;
    }

    // RIFF 컨테이너: 0..3 = "RIFF", 8..11 = "WEBP"
    bool is_webp(const uint8_t* p, size_t n) {
        return n >= 12 && std::memcmp(p, "RIFF", 4) == 0 &&
               std::memcmp(p + 8, "WEBP", 4) == 0;
    }

    // "GIF87a" 또는 "GIF89a"
    bool is_gif(const uint8_t* p, size_t n) {
        return n >= 6 && std::memcmp(p, "GIF8", 4) == 0 &&
               (p[4] == '7' || p[4] == '9') && p[5] == 'a';
    }

    // TIFF: "II*\0"(리틀엔디안) 또는 "MM\0*"(빅엔디안)
    bool is_tiff(const uint8_t* p, size_t n) {
        if (n < 4) {
            return false;
        }
        return (p[0] == 'I' && p[1] == 'I' && p[2] == 0x2A && p[3] == 0x00) ||
               (p[0] == 'M' && p[1] == 'M' && p[2] == 0x00 && p[3] == 0x2A);
    }

    // QOI: "qoif"
    bool is_qoi(const uint8_t* p, size_t n) {
        return n >= 4 && std::memcmp(p, "qoif", 4) == 0;
    }

    // BMP: "BM"
    bool is_bmp(const uint8_t* p, size_t n) {
        return n >= 2 && p[0] == 'B' && p[1] == 'M';
    }

    // PNM 계열: "P1"~"P6" 다음에 공백 또는 주석('#')
    bool is_pnm(const uint8_t* p, size_t n) {
        if (n < 3 || p[0] != 'P' || p[1] < '1' || p[1] > '6') {
            return false;
        }
        const uint8_t c = p[2];
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#';
    }

    // OpenEXR: 매직 넘버 0x76 0x2F 0x31 0x01
    bool is_exr(const uint8_t* p, size_t n) {
        return n >= 4 && p[0] == 0x76 && p[1] == 0x2F && p[2] == 0x31 && p[3] == 0x01;
    }

    // TGA 는 파일 시그니처가 없어 확장자로 판별한다.
    bool has_tga_extension(const std::string& path) {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) {
            return false;
        }
        std::string ext = path.substr(dot + 1);
        for (char& c : ext) {
            c = (char)std::tolower((unsigned char)c);
        }
        return ext == "tga";
    }

    // JPEG XL: 원시 코드스트림(FF 0A) 또는 ISOBMFF 컨테이너 시그니처
    bool is_jxl(const uint8_t* p, size_t n) {
        if (n >= 2 && p[0] == 0xFF && p[1] == 0x0A) {
            return true;
        }
        static const uint8_t box[12] = {0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ',
                                        0x0D, 0x0A, 0x87, 0x0A};
        return n >= 12 && std::memcmp(p, box, 12) == 0;
    }

    // ISOBMFF 컨테이너(4..7 = "ftyp")의 major/compatible 브랜드 중 하나라도 일치하는지 본다.
    bool ftyp_has_brand(const uint8_t* p, size_t n,
                        const char* const* brands, size_t brandCount) {
        if (n < 12 || std::memcmp(p + 4, "ftyp", 4) != 0) {
            return false;
        }
        // ftyp 박스 크기(빅엔디안) 범위 내에서 브랜드들을 스캔
        const uint32_t boxSize = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                 (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        size_t limit = n;
        if (boxSize >= 8 && boxSize < n) {
            limit = boxSize;
        }
        for (size_t off = 8; off + 4 <= limit; off += 4) {
            for (size_t i = 0; i < brandCount; ++i) {
                if (std::memcmp(p + off, brands[i], 4) == 0) {
                    return true;
                }
            }
        }
        return false;
    }

    // AVIF: ISOBMFF 컨테이너의 브랜드가 "avif"(정지) 또는 "avis"(시퀀스).
    bool is_avif(const uint8_t* p, size_t n) {
        static const char* const brands[] = {"avif", "avis"};
        return ftyp_has_brand(p, n, brands, std::size(brands));
    }

    // HEIF/HEIC: HEVC 기반 브랜드들과 범용 이미지/시퀀스 브랜드(mif1/msf1/miaf).
    // AVIF 도 mif1 을 함께 갖는 경우가 있으므로 is_avif() 를 먼저 확인해야 한다.
    bool is_heif(const uint8_t* p, size_t n) {
        static const char* const brands[] = {
            "heic", "heix", "heim", "heis",   // HEVC 정지 이미지
            "hevc", "hevx", "hevm", "hevs",   // HEVC 이미지 시퀀스
            "mif1", "mif2", "msf1", "miaf",   // 범용 HEIF 이미지/시퀀스
        };
        return ftyp_has_brand(p, n, brands, std::size(brands));
    }
} // namespace

DecodedImage decode_image(const std::string& path) {
    uint8_t magic[32] = {0};
    const size_t n = read_magic(path, magic, sizeof(magic));

    if (is_png(magic, n)) {
        return decode_png(path);
    }
    if (is_jpeg(magic, n)) {
        return decode_jpeg(path);
    }
    if (is_jpeg2000(magic, n)) {
        return decode_jpeg2000(path);
    }
    if (is_webp(magic, n)) {
        return decode_webp(path);
    }
    if (is_gif(magic, n)) {
        return decode_gif(path);
    }
    if (is_tiff(magic, n)) {
        return decode_tiff(path);
    }
    if (is_avif(magic, n)) {   // AVIF 는 HEIF 와 컨테이너가 같으므로 먼저 판별
        return decode_avif(path);
    }
    if (is_heif(magic, n)) {
        return decode_heif(path);
    }
    if (is_qoi(magic, n)) {
        return decode_qoi(path);
    }
    if (is_jxl(magic, n)) {
        return decode_jxl(path);
    }
    if (is_bmp(magic, n)) {
        return decode_stb(path);
    }
    if (is_pnm(magic, n)) {
        return decode_pnm(path);
    }
    if (is_exr(magic, n)) {
        return decode_exr(path);
    }
    if (has_tga_extension(path)) {   // TGA 는 시그니처가 없어 확장자로 판별
        return decode_stb(path);
    }

    DecodedImage img;
    if (n == 0) {
        img.error = "파일을 열 수 없습니다: " + path;
    } else {
        img.error = "지원하지 않는 이미지 형식입니다"
                    "(JPEG/PNG/WebP/GIF/TIFF/AVIF/HEIF/QOI/JPEG XL/JPEG 2000/BMP/TGA/PNM/EXR 만 지원): " + path;
    }
    return img;
}

DecodedImage decode_preview(const std::string& path) {
    uint8_t magic[32] = {0};
    const size_t n = read_magic(path, magic, sizeof(magic));

    // 현재는 JPEG 만 빠른 저해상도 미리보기를 지원한다.
    if (is_jpeg(magic, n)) {
        return decode_jpeg_preview(path);
    }

    DecodedImage img;   // ok=false → 호출자는 미리보기 없이 전체 디코딩으로 진행
    return img;
}
