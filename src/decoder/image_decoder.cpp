//
// image_decoder.cpp
// 파일 시그니처(매직 바이트)를 보고 포맷별 디코더로 분기한다.
//
#include "decoder/image_decoder.h"

#include "decoder/jpeg_decoder.h"
#include "decoder/png_decoder.h"
#include "decoder/webp_decoder.h"
#include "decoder/gif_decoder.h"
#include "decoder/tiff_decoder.h"
#include "decoder/avif_decoder.h"
#include "decoder/qoi_decoder.h"
#include "decoder/jxl_decoder.h"
#include "decoder/jpeg2000_decoder.h"
#include "decoder/stb_decoder.h"
#include "decoder/psd_decoder.h"
#include "decoder/ico_decoder.h"
#include "decoder/tga_decoder.h"
#include "decoder/svg_decoder.h"
#include "decoder/pnm_decoder.h"
#include "decoder/exr_decoder.h"
#include "decoder/heif_decoder.h"
#include "decoder/mediainfo.h"
#include "thread_count.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>

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

    // Photoshop: signature "8BPS", version 필드는 그 뒤에 온다.
    bool is_psd(const uint8_t* p, size_t n) {
        return n >= 4 && std::memcmp(p, "8BPS", 4) == 0;
    }

    // Windows icon: reserved=0, type=1, image count > 0 (모두 little-endian).
    bool is_ico(const uint8_t* p, size_t n) {
        return n >= 6 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0 &&
               (p[4] != 0 || p[5] != 0);
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

    bool has_extension(const std::string& path, const char* expected) {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) {
            return false;
        }
        std::string ext = path.substr(dot + 1);
        for (char& c : ext) {
            c = (char)std::tolower((unsigned char)c);
        }
        return ext == expected;
    }

    // SVG는 XML 선언/공백으로 시작할 수 있다. 앞부분의 실제 <svg 루트 태그를 찾는다.
    bool is_svg(const uint8_t* p, size_t n) {
        for (size_t i = 0; i + 4 <= n; ++i) {
            if (p[i] == '<' && p[i + 1] == 's' && p[i + 2] == 'v' && p[i + 3] == 'g') {
                if (i + 4 == n || std::isspace(static_cast<unsigned char>(p[i + 4])) ||
                    p[i + 4] == '>' || p[i + 4] == '/') {
                    return true;
                }
            }
        }
        return false;
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

    bool contains_xyb(const std::string& value) {
        for (size_t i = 0; i + 3 <= value.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(value[i])) == 'X' &&
                std::toupper(static_cast<unsigned char>(value[i + 1])) == 'Y' &&
                std::toupper(static_cast<unsigned char>(value[i + 2])) == 'B') {
                return true;
            }
        }
        return false;
    }

    bool jpeg_has_xyb_icc_profile(const std::string& path) {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }

        const int soi0 = std::fgetc(file);
        const int soi1 = std::fgetc(file);
        if (soi0 != 0xFF || soi1 != 0xD8) {
            std::fclose(file);
            return false;
        }

        bool isXyb = false;
        while (!isXyb) {
            int markerPrefix = std::fgetc(file);
            while (markerPrefix != EOF && markerPrefix != 0xFF) {
                markerPrefix = std::fgetc(file);
            }
            if (markerPrefix == EOF) {
                break;
            }

            int marker = std::fgetc(file);
            while (marker == 0xFF) {
                marker = std::fgetc(file);
            }
            if (marker == EOF || marker == 0xD9 || marker == 0xDA) {
                break; // EOF, EOI 또는 압축 데이터(SOS) 시작
            }
            if (marker == 0x00 || marker == 0x01 ||
                (marker >= 0xD0 && marker <= 0xD7)) {
                continue; // byte stuffing, TEM, restart marker
            }

            const int lengthHigh = std::fgetc(file);
            const int lengthLow = std::fgetc(file);
            if (lengthHigh == EOF || lengthLow == EOF) {
                break;
            }
            const size_t segmentLength =
                (static_cast<size_t>(lengthHigh) << 8) | static_cast<size_t>(lengthLow);
            if (segmentLength < 2) {
                break;
            }
            const size_t payloadSize = segmentLength - 2;

            if (marker != 0xE2) { // APP2만 ICC 프로파일을 담는다.
                if (std::fseek(file, static_cast<long>(payloadSize), SEEK_CUR) != 0) {
                    break;
                }
                continue;
            }

            std::vector<uint8_t> payload(payloadSize);
            if (std::fread(payload.data(), 1, payload.size(), file) != payload.size()) {
                break;
            }
            static constexpr char iccSignature[] = "ICC_PROFILE";
            if (payload.size() < 14 ||
                std::memcmp(payload.data(), iccSignature, sizeof(iccSignature)) != 0) {
                continue;
            }

            // APP2의 14바이트 ICC 헤더 뒤가 프로파일 데이터다. jpegli/libjxl이
            // 만든 XYB 프로파일은 ICC preferred CMM type(bytes 4..7)이 "jxl "이다.
            const size_t profileOffset = 14;
            if (payload.size() >= profileOffset + 8 &&
                std::memcmp(payload.data() + profileOffset + 4, "jxl ", 4) == 0) {
                isXyb = true;
                break;
            }

            // 분할 ICC나 다른 생성기의 프로파일을 위해 ASCII/UTF-16BE 표기도 확인한다.
            for (size_t i = profileOffset; i + 3 <= payload.size(); ++i) {
                if (std::toupper(payload[i]) == 'X' &&
                    std::toupper(payload[i + 1]) == 'Y' &&
                    std::toupper(payload[i + 2]) == 'B') {
                    isXyb = true;
                    break;
                }
                if (i + 6 <= payload.size() && payload[i] == 0 && payload[i + 1] == 'X' &&
                    payload[i + 2] == 0 && payload[i + 3] == 'Y' &&
                    payload[i + 4] == 0 && payload[i + 5] == 'B') {
                    isXyb = true;
                    break;
                }
            }
        }

        std::fclose(file);
        return isXyb;
    }

    uint8_t clipped_u8(float value) {
        if (value <= 0.0f) {
            return 0;
        }
        if (value >= 255.0f) {
            return 255;
        }
        // NumPy의 astype(np.uint8)와 동일하게 소수점 이하는 버린다.
        return static_cast<uint8_t>(value);
    }

    void jpegli_xyb_to_rgba(DecodedImage& image, bool mt) {
        if (!image.ok || image.pixels.size() < 4) {
            return;
        }

        const size_t pixelCount = image.pixels.size() / 4;
        auto transformRange = [&image](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                uint8_t* pixel = image.pixels.data() + i * 4;

                // OpenCV 입력/출력은 BGR 순서지만 DecodedImage는 RGBA 순서다.
                const float x = static_cast<float>(pixel[2]);
                const float y = static_cast<float>(pixel[1]);
                const float b = static_cast<float>(pixel[0]);

                const float outBlue  = 1.78f * x + 0.98f * y - 0.45f * b - 148.5f;
                const float outGreen = 0.07f * x + 0.98f * y - 0.47f * b + 34.8f;
                const float outRed   = 0.11f * x + 1.00f * y + 0.87f * b - 91.1f;

                pixel[0] = clipped_u8(outRed);
                pixel[1] = clipped_u8(outGreen);
                pixel[2] = clipped_u8(outBlue);
            }
        };

        const size_t workerCount = mt
            ? std::min<size_t>(decoder_detail::available_thread_count(), pixelCount)
            : 1;
        if (workerCount <= 1) {
            transformRange(0, pixelCount);
            return;
        }

        const size_t chunkSize = (pixelCount + workerCount - 1) / workerCount;
        std::vector<std::thread> workers;
        workers.reserve(workerCount - 1);
        for (size_t worker = 1; worker < workerCount; ++worker) {
            const size_t begin = worker * chunkSize;
            const size_t end = std::min(begin + chunkSize, pixelCount);
            if (begin < end) {
                workers.emplace_back(transformRange, begin, end);
            }
        }
        transformRange(0, std::min(chunkSize, pixelCount));
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
} // namespace

DecodedImage decode_image(const std::string& path,bool mt) {
    uint8_t magic[512] = {0};
    const size_t n = read_magic(path, magic, sizeof(magic));

    if (is_png(magic, n)) {
        return decode_png(path);
    }
    if (is_jpeg(magic, n)) {
        const auto info = get_mediainfo(path);
        const auto description = info.find("Image.colour_primaries_ICC_Description");
        const bool isXyb =
            (description != info.end() && contains_xyb(description->second)) ||
            jpeg_has_xyb_icc_profile(path);

        DecodedImage result = decode_jpeg(path);
        if (isXyb) {
            jpegli_xyb_to_rgba(result, mt);
        }
        return result;
    }
    if (is_jpeg2000(magic, n)) {
        return decode_jpeg2000(path, mt);
    }
    if (is_webp(magic, n)) {
        return decode_webp(path, mt);
    }
    if (is_gif(magic, n)) {
        return decode_gif(path);
    }
    if (is_tiff(magic, n)) {
        return decode_tiff(path);
    }
    if (is_avif(magic, n)) {   // AVIF 는 HEIF 와 컨테이너가 같으므로 먼저 판별
        return decode_avif(path, mt);
    }
    if (is_heif(magic, n)) {
        return decode_heif(path, mt);
    }
    if (is_qoi(magic, n)) {
        return decode_qoi(path);
    }
    if (is_jxl(magic, n)) {
        return decode_jxl(path, mt);
    }
    if (is_bmp(magic, n)) {
        return decode_stb(path);
    }
    if (is_psd(magic, n)) {
        return decode_psd(path);
    }
    if (is_ico(magic, n)) {
        return decode_ico(path);
    }
    if (is_pnm(magic, n)) {
        return decode_pnm(path);
    }
    if (is_exr(magic, n)) {
        return decode_exr(path, mt);
    }
    if (is_svg(magic, n) || has_extension(path, "svg") || has_extension(path, "svgz")) {
        return decode_svg(path);
    }
    if (has_extension(path, "tga")) {   // TGA 는 고유 시그니처가 없어 확장자로 판별
        return decode_tga(path);
    }

    DecodedImage img;
    if (n == 0) {
        img.error = "파일을 열 수 없습니다: " + path;
    } else {
        img.error = "지원하지 않는 이미지 형식입니다"
                    "(JPEG/PNG/WebP/GIF/TIFF/AVIF/HEIF/QOI/JPEG XL/JPEG 2000/"
                    "BMP/PSD/ICO/TGA/SVG/PNM/EXR 만 지원): " + path;
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
