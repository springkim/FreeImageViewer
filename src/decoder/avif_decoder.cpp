//
// avif_decoder.cpp
// libavif 로 AVIF 를 8비트 RGBA 로 디코딩한다.
// 정적 이미지는 물론, 이미지 시퀀스(애니메이션)면 프레임별 지연과 함께 모두 디코딩한다.
//
#include "decoder/avif_decoder.h"
#include "thread_count.h"

#include <avif/avif.h>

#include <cmath>
#include <cstring>

DecodedImage decode_avif(const std::string& path, bool mt) {
    DecodedImage img;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) {
        img.error = "avifDecoderCreate 실패";
        return img;
    }
    const int threadCount = mt ? decoder_detail::available_thread_count() : 1;
    decoder->maxThreads = threadCount;

    avifResult res = avifDecoderSetIOFile(decoder, path.c_str());
    if (res != AVIF_RESULT_OK) {
        img.error = std::string("AVIF 열기 실패: ") + avifResultToString(res);
        avifDecoderDestroy(decoder);
        return img;
    }

    res = avifDecoderParse(decoder);
    if (res != AVIF_RESULT_OK) {
        img.error = std::string("AVIF 파싱 실패: ") + avifResultToString(res);
        avifDecoderDestroy(decoder);
        return img;
    }

    const int W = static_cast<int>(decoder->image->width);
    const int H = static_cast<int>(decoder->image->height);
    if (W <= 0 || H <= 0) {
        img.error = "유효하지 않은 AVIF 크기입니다";
        avifDecoderDestroy(decoder);
        return img;
    }
    img.width  = W;
    img.height = H;

    // 프레임을 순회하며 각각 RGBA 로 변환(정적 이미지는 1회 반복).
    while ((res = avifDecoderNextImage(decoder)) == AVIF_RESULT_OK) {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, decoder->image);
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = 8;
        rgb.maxThreads = threadCount;

        if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
            img.error = "AVIF RGB 버퍼 할당 실패";
            img.frames.clear();
            avifDecoderDestroy(decoder);
            return img;
        }

        avifResult conv = avifImageYUVToRGB(decoder->image, &rgb);
        if (conv != AVIF_RESULT_OK) {
            img.error = std::string("AVIF 색변환 실패: ") + avifResultToString(conv);
            avifRGBImageFreePixels(&rgb);
            img.frames.clear();
            avifDecoderDestroy(decoder);
            return img;
        }

        // rowBytes(패딩)를 고려해 한 행씩 복사.
        ImageFrame frame;
        frame.pixels.resize(static_cast<size_t>(W) * H * 4);
        for (int y = 0; y < H; ++y) {
            std::memcpy(&frame.pixels[static_cast<size_t>(y) * W * 4],
                        rgb.pixels + static_cast<size_t>(y) * rgb.rowBytes,
                        static_cast<size_t>(W) * 4);
        }
        frame.delay_ms = static_cast<int>(std::lround(decoder->imageTiming.duration * 1000.0));

        avifRGBImageFreePixels(&rgb);
        img.frames.push_back(std::move(frame));
    }

    avifDecoderDestroy(decoder);

    // 마지막이 '남은 이미지 없음'이면 정상 종료, 그 외 코드면 디코딩 도중 오류.
    if (img.frames.empty()) {
        if (img.error.empty()) {
            img.error = std::string("AVIF 디코딩 실패: ") + avifResultToString(res);
        }
        return img;
    }

    img.pixels = img.frames.front().pixels;   // 첫 프레임(정적 표시/폴백용)
    img.ok = true;
    return img;
}
