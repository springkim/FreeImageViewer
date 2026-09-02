//
// webp_decoder.cpp
// libwebp 로 정적 WebP와 Animated WebP를 RGBA 로 디코딩한다.
//
#include "decoder/webp_decoder.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <cstdio>
#include <cstring>

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

    bool decode_animation(const std::vector<uint8_t>& webp, bool mt,
                          DecodedImage& img) {
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options)) {
            img.error = "Animated WebP 디코더 설정 초기화 실패";
            return false;
        }
        options.color_mode = MODE_RGBA;
        options.use_threads = mt ? 1 : 0;

        const WebPData data = {webp.data(), webp.size()};
        WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
        if (!decoder) {
            img.error = "Animated WebP 디코더 생성 실패";
            return false;
        }

        WebPAnimInfo info{};
        if (!WebPAnimDecoderGetInfo(decoder, &info) ||
            info.canvas_width == 0 || info.canvas_height == 0) {
            img.error = "Animated WebP 정보를 읽지 못했습니다";
            WebPAnimDecoderDelete(decoder);
            return false;
        }

        img.width = static_cast<int>(info.canvas_width);
        img.height = static_cast<int>(info.canvas_height);
        const size_t frameSize = static_cast<size_t>(img.width) * img.height * 4;
        int previousTimestamp = 0;

        while (WebPAnimDecoderHasMoreFrames(decoder)) {
            uint8_t* rgba = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(decoder, &rgba, &timestamp) || !rgba ||
                timestamp < previousTimestamp) {
                img.error = "Animated WebP 프레임 디코딩 실패";
                img.frames.clear();
                WebPAnimDecoderDelete(decoder);
                return false;
            }

            ImageFrame frame;
            frame.pixels.assign(rgba, rgba + frameSize);
            frame.delay_ms = timestamp - previousTimestamp;
            img.frames.push_back(std::move(frame));
            previousTimestamp = timestamp;
        }

        WebPAnimDecoderDelete(decoder);
        if (img.frames.size() != info.frame_count || img.frames.empty()) {
            img.error = "Animated WebP 프레임 수가 올바르지 않습니다";
            img.frames.clear();
            return false;
        }

        img.pixels = img.frames.front().pixels;
        img.ok = true;
        return true;
    }
} // namespace

DecodedImage decode_webp(const std::string& path, bool mt) {
    DecodedImage img;

    std::vector<uint8_t> webp;
    if (!read_file(path, webp, img.error)) {
        return img;
    }

    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(webp.data(), webp.size(), &features) != VP8_STATUS_OK) {
        img.error = "WebP 헤더 파싱 실패";
        return img;
    }
    if (features.has_animation) {
        decode_animation(webp, mt, img);
        return img;
    }

    WebPDecoderConfig config{};
    if (!WebPInitDecoderConfig(&config)) {
        img.error = "WebP 디코더 설정 초기화 실패";
        return img;
    }
    config.output.colorspace = MODE_RGBA;
    config.options.use_threads = mt ? 1 : 0;

    const VP8StatusCode status = WebPDecode(webp.data(), webp.size(), &config);
    if (status != VP8_STATUS_OK) {
        img.error = "WebP 디코딩 실패(손상되었거나 지원하지 않는 WebP)";
        WebPFreeDecBuffer(&config.output);
        return img;
    }
    const int width = config.output.width;
    const int height = config.output.height;
    const uint8_t* rgba = config.output.u.RGBA.rgba;
    const int stride = config.output.u.RGBA.stride;
    if (width <= 0 || height <= 0) {
        img.error = "유효하지 않은 이미지 크기입니다";
        WebPFreeDecBuffer(&config.output);
        return img;
    }

    img.width  = width;
    img.height = height;
    img.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(img.pixels.data() + static_cast<size_t>(y) * width * 4,
                    rgba + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(width) * 4);
    }

    WebPFreeDecBuffer(&config.output);
    img.ok = true;
    return img;
}
