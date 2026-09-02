//
// png_decoder.cpp
// libspng 로 정적 PNG와 APNG를 RGBA8 로 디코딩한다. APNG는 각 프레임의
// 압축 데이터를 독립 PNG로 재구성해 libspng로 푼 뒤 캔버스에 합성한다.
//
#include "decoder/png_decoder.h"

#include <spng.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
    constexpr std::array<uint8_t, 8> pngSignature = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };

    struct ApngFrameControl {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t xOffset = 0;
        uint32_t yOffset = 0;
        uint16_t delayNumerator = 0;
        uint16_t delayDenominator = 0;
        uint8_t disposeOp = 0;
        uint8_t blendOp = 0;
    };

    struct ApngFrame {
        ApngFrameControl control;
        std::vector<std::vector<uint8_t>> compressedData;
    };

    uint16_t read_be16(const uint8_t* data) {
        return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                     static_cast<uint16_t>(data[1]));
    }

    uint32_t read_be32(const uint8_t* data) {
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
               static_cast<uint32_t>(data[3]);
    }

    void append_be32(std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(static_cast<uint8_t>(value >> 24));
        out.push_back(static_cast<uint8_t>(value >> 16));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value));
    }

    uint32_t png_crc32(const uint8_t* data, size_t size) {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                const uint32_t mask = 0u - (crc & 1u);
                crc = (crc >> 1) ^ (0xedb88320u & mask);
            }
        }
        return crc ^ 0xffffffffu;
    }

    void append_chunk(std::vector<uint8_t>& png, const char type[4],
                      const uint8_t* data, size_t size) {
        append_be32(png, static_cast<uint32_t>(size));
        const size_t crcBegin = png.size();
        png.insert(png.end(), type, type + 4);
        if (size > 0) {
            png.insert(png.end(), data, data + size);
        }
        append_be32(png, png_crc32(png.data() + crcBegin, size + 4));
    }

    bool read_file(const std::string& path, std::vector<uint8_t>& out,
                   std::string& error) {
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

    bool decode_png_rgba(const std::vector<uint8_t>& png, int& width, int& height,
                         std::vector<uint8_t>& pixels, std::string& error) {
        spng_ctx* ctx = spng_ctx_new(0);
        if (!ctx) {
            error = "spng_ctx_new 실패";
            return false;
        }

        int ret = spng_set_png_buffer(ctx, png.data(), png.size());
        if (ret != 0) {
            error = std::string("spng_set_png_buffer 실패: ") + spng_strerror(ret);
            spng_ctx_free(ctx);
            return false;
        }

        spng_ihdr ihdr{};
        ret = spng_get_ihdr(ctx, &ihdr);
        if (ret != 0 || ihdr.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            ihdr.height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            error = ret != 0 ? std::string("PNG 헤더 파싱 실패: ") + spng_strerror(ret)
                             : "PNG 크기가 너무 큽니다";
            spng_ctx_free(ctx);
            return false;
        }

        size_t outSize = 0;
        ret = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &outSize);
        if (ret != 0) {
            error = std::string("이미지 크기 계산 실패: ") + spng_strerror(ret);
            spng_ctx_free(ctx);
            return false;
        }

        pixels.resize(outSize);
        ret = spng_decode_image(ctx, pixels.data(), outSize,
                                SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
        if (ret != 0) {
            error = std::string("PNG 디코딩 실패: ") + spng_strerror(ret);
            spng_ctx_free(ctx);
            return false;
        }

        width = static_cast<int>(ihdr.width);
        height = static_cast<int>(ihdr.height);
        spng_ctx_free(ctx);
        return true;
    }

    bool is_chunk(const uint8_t* type, const char expected[5]) {
        return std::memcmp(type, expected, 4) == 0;
    }

    bool parse_apng(const std::vector<uint8_t>& png, std::array<uint8_t, 13>& ihdr,
                    std::vector<std::vector<uint8_t>>& sharedChunks,
                    std::vector<ApngFrame>& frames, uint32_t& declaredFrameCount,
                    std::string& error) {
        if (png.size() < pngSignature.size() ||
            !std::equal(pngSignature.begin(), pngSignature.end(), png.begin())) {
            error = "올바른 PNG 시그니처가 아닙니다";
            return false;
        }

        bool haveIhdr = false;
        bool haveAnimation = false;
        bool seenImageData = false;
        size_t offset = pngSignature.size();

        while (offset <= png.size() && png.size() - offset >= 12) {
            const uint32_t length = read_be32(png.data() + offset);
            if (length > png.size() - offset - 12) {
                error = "PNG 청크가 파일 범위를 벗어났습니다";
                return false;
            }

            const uint8_t* type = png.data() + offset + 4;
            const uint8_t* data = png.data() + offset + 8;
            const size_t chunkSize = static_cast<size_t>(length) + 12;

            if (is_chunk(type, "IHDR")) {
                if (length != ihdr.size() || haveIhdr) {
                    error = "올바르지 않은 PNG IHDR 청크입니다";
                    return false;
                }
                std::copy(data, data + ihdr.size(), ihdr.begin());
                haveIhdr = true;
            } else if (is_chunk(type, "acTL")) {
                if (length != 8 || haveAnimation) {
                    error = "올바르지 않은 APNG acTL 청크입니다";
                    return false;
                }
                declaredFrameCount = read_be32(data);
                haveAnimation = true;
            } else if (is_chunk(type, "fcTL")) {
                if (!haveAnimation || length != 26) {
                    error = "올바르지 않은 APNG fcTL 청크입니다";
                    return false;
                }
                ApngFrame frame;
                frame.control.width = read_be32(data + 4);
                frame.control.height = read_be32(data + 8);
                frame.control.xOffset = read_be32(data + 12);
                frame.control.yOffset = read_be32(data + 16);
                frame.control.delayNumerator = read_be16(data + 20);
                frame.control.delayDenominator = read_be16(data + 22);
                frame.control.disposeOp = data[24];
                frame.control.blendOp = data[25];
                frames.push_back(std::move(frame));
            } else if (is_chunk(type, "IDAT")) {
                seenImageData = true;
                // fcTL이 IDAT보다 먼저 나온 경우 기본 이미지는 애니메이션의 첫 프레임이다.
                if (!frames.empty()) {
                    frames.back().compressedData.emplace_back(data, data + length);
                }
            } else if (is_chunk(type, "fdAT")) {
                seenImageData = true;
                if (frames.empty() || length < 4) {
                    error = "fcTL 없는 APNG fdAT 청크입니다";
                    return false;
                }
                // 앞의 4바이트 sequence_number를 제외하면 IDAT와 같은 데이터다.
                frames.back().compressedData.emplace_back(data + 4, data + length);
            } else if (is_chunk(type, "IEND")) {
                break;
            } else if (!seenImageData) {
                // PLTE/tRNS/색상 프로파일 등 프레임 디코딩에 필요한 공용 청크.
                sharedChunks.emplace_back(png.begin() + static_cast<ptrdiff_t>(offset),
                                          png.begin() + static_cast<ptrdiff_t>(offset + chunkSize));
            }

            offset += chunkSize;
        }

        if (!haveIhdr) {
            error = "PNG IHDR 청크가 없습니다";
            return false;
        }
        return haveAnimation;
    }

    std::vector<uint8_t> make_frame_png(const std::array<uint8_t, 13>& sourceIhdr,
                                        const std::vector<std::vector<uint8_t>>& sharedChunks,
                                        const ApngFrame& frame) {
        std::vector<uint8_t> png(pngSignature.begin(), pngSignature.end());
        std::array<uint8_t, 13> frameIhdr = sourceIhdr;
        frameIhdr[0] = static_cast<uint8_t>(frame.control.width >> 24);
        frameIhdr[1] = static_cast<uint8_t>(frame.control.width >> 16);
        frameIhdr[2] = static_cast<uint8_t>(frame.control.width >> 8);
        frameIhdr[3] = static_cast<uint8_t>(frame.control.width);
        frameIhdr[4] = static_cast<uint8_t>(frame.control.height >> 24);
        frameIhdr[5] = static_cast<uint8_t>(frame.control.height >> 16);
        frameIhdr[6] = static_cast<uint8_t>(frame.control.height >> 8);
        frameIhdr[7] = static_cast<uint8_t>(frame.control.height);
        append_chunk(png, "IHDR", frameIhdr.data(), frameIhdr.size());
        for (const auto& chunk : sharedChunks) {
            png.insert(png.end(), chunk.begin(), chunk.end());
        }
        for (const auto& data : frame.compressedData) {
            append_chunk(png, "IDAT", data.data(), data.size());
        }
        append_chunk(png, "IEND", nullptr, 0);
        return png;
    }

    void clear_frame_rect(std::vector<uint8_t>& canvas, int canvasWidth,
                          const ApngFrameControl& control) {
        for (uint32_t y = 0; y < control.height; ++y) {
            const size_t begin = (static_cast<size_t>(control.yOffset + y) * canvasWidth +
                                  control.xOffset) * 4;
            std::fill(canvas.begin() + static_cast<ptrdiff_t>(begin),
                      canvas.begin() + static_cast<ptrdiff_t>(begin + control.width * 4), 0);
        }
    }

    void composite_frame(std::vector<uint8_t>& canvas, int canvasWidth,
                         const ApngFrameControl& control,
                         const std::vector<uint8_t>& source) {
        for (uint32_t y = 0; y < control.height; ++y) {
            for (uint32_t x = 0; x < control.width; ++x) {
                const size_t srcOffset = (static_cast<size_t>(y) * control.width + x) * 4;
                const size_t dstOffset =
                    (static_cast<size_t>(control.yOffset + y) * canvasWidth +
                     control.xOffset + x) * 4;
                const uint8_t* src = source.data() + srcOffset;
                uint8_t* dst = canvas.data() + dstOffset;

                if (control.blendOp == 0) { // APNG_BLEND_OP_SOURCE
                    std::memcpy(dst, src, 4);
                    continue;
                }

                // APNG_BLEND_OP_OVER, straight-alpha RGBA 합성.
                const uint32_t srcAlpha = src[3];
                const uint32_t dstAlpha = dst[3];
                const uint32_t inverseSrcAlpha = 255 - srcAlpha;
                const uint32_t alphaNumerator =
                    srcAlpha * 255 + dstAlpha * inverseSrcAlpha;
                if (alphaNumerator == 0) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                for (int channel = 0; channel < 3; ++channel) {
                    const uint32_t colorNumerator =
                        src[channel] * srcAlpha * 255 +
                        dst[channel] * dstAlpha * inverseSrcAlpha;
                    dst[channel] = static_cast<uint8_t>(
                        (colorNumerator + alphaNumerator / 2) / alphaNumerator);
                }
                dst[3] = static_cast<uint8_t>((alphaNumerator + 127) / 255);
            }
        }
    }

    bool decode_apng(const std::vector<uint8_t>& png, DecodedImage& img) {
        std::array<uint8_t, 13> ihdr{};
        std::vector<std::vector<uint8_t>> sharedChunks;
        std::vector<ApngFrame> frames;
        uint32_t declaredFrameCount = 0;
        if (!parse_apng(png, ihdr, sharedChunks, frames, declaredFrameCount, img.error)) {
            return false;
        }

        const uint32_t canvasWidth = read_be32(ihdr.data());
        const uint32_t canvasHeight = read_be32(ihdr.data() + 4);
        if (canvasWidth == 0 || canvasHeight == 0 ||
            canvasWidth > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            canvasHeight > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            static_cast<size_t>(canvasWidth) >
                std::numeric_limits<size_t>::max() / canvasHeight / 4) {
            img.error = "유효하지 않은 APNG 캔버스 크기입니다";
            return false;
        }
        if (declaredFrameCount == 0 || frames.size() != declaredFrameCount) {
            img.error = "APNG 프레임 수가 acTL 정보와 일치하지 않습니다";
            return false;
        }

        img.width = static_cast<int>(canvasWidth);
        img.height = static_cast<int>(canvasHeight);
        std::vector<uint8_t> canvas(static_cast<size_t>(canvasWidth) * canvasHeight * 4, 0);

        for (const ApngFrame& frame : frames) {
            const ApngFrameControl& control = frame.control;
            if (control.width == 0 || control.height == 0 ||
                static_cast<uint64_t>(control.xOffset) + control.width > canvasWidth ||
                static_cast<uint64_t>(control.yOffset) + control.height > canvasHeight ||
                control.disposeOp > 2 || control.blendOp > 1 ||
                frame.compressedData.empty()) {
                img.error = "올바르지 않은 APNG 프레임 정보입니다";
                img.frames.clear();
                return false;
            }

            const std::vector<uint8_t> framePng = make_frame_png(ihdr, sharedChunks, frame);
            int frameWidth = 0;
            int frameHeight = 0;
            std::vector<uint8_t> framePixels;
            if (!decode_png_rgba(framePng, frameWidth, frameHeight, framePixels, img.error) ||
                frameWidth != static_cast<int>(control.width) ||
                frameHeight != static_cast<int>(control.height)) {
                if (img.error.empty()) {
                    img.error = "APNG 프레임 크기가 올바르지 않습니다";
                }
                img.frames.clear();
                return false;
            }

            std::vector<uint8_t> previousCanvas;
            if (control.disposeOp == 2) { // APNG_DISPOSE_OP_PREVIOUS
                previousCanvas = canvas;
            }
            composite_frame(canvas, img.width, control, framePixels);

            ImageFrame outputFrame;
            outputFrame.pixels = canvas;
            const uint32_t denominator =
                control.delayDenominator == 0 ? 100 : control.delayDenominator;
            outputFrame.delay_ms = static_cast<int>(std::lround(
                static_cast<double>(control.delayNumerator) * 1000.0 / denominator));
            img.frames.push_back(std::move(outputFrame));

            if (control.disposeOp == 1) { // APNG_DISPOSE_OP_BACKGROUND
                clear_frame_rect(canvas, img.width, control);
            } else if (control.disposeOp == 2) {
                canvas = std::move(previousCanvas);
            }
        }

        img.pixels = img.frames.front().pixels;
        img.ok = true;
        return true;
    }
} // namespace

DecodedImage decode_png(const std::string& path) {
    DecodedImage img;

    std::vector<uint8_t> png;
    if (!read_file(path, png, img.error)) {
        return img;
    }

    // acTL이 있으면 APNG로 파싱한다. 일반 PNG 경로는 기존 libspng 단일 디코딩을 유지한다.
    bool hasActl = false;
    for (size_t offset = pngSignature.size(); offset <= png.size() &&
         png.size() - offset >= 12;) {
        const uint32_t length = read_be32(png.data() + offset);
        if (length > png.size() - offset - 12) {
            break;
        }
        if (is_chunk(png.data() + offset + 4, "acTL")) {
            hasActl = true;
            break;
        }
        offset += static_cast<size_t>(length) + 12;
    }
    if (hasActl) {
        decode_apng(png, img);
        return img;
    }

    if (!decode_png_rgba(png, img.width, img.height, img.pixels, img.error)) {
        return img;
    }
    img.ok = true;
    return img;
}
