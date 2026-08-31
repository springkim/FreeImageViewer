//
// jxl_decoder.cpp
// libjxl 로 JPEG XL 을 8비트 RGBA 로 디코딩한다.
// 정적 이미지는 물론, 애니메이션이면 프레임별 지연과 함께 모두 디코딩한다.
//
#include "jxl_decoder.h"

#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <hwy/targets.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {
    void configure_highway_targets() {
#if defined(_WIN32) && defined(__GNUC__) && defined(HWY_AVX2)
        // The bundled MinGW build of libjxl crashes in Highway's AVX2
        // WriteToOutputStage for otherwise valid images.  Disable only that
        // runtime target; Highway will select another supported SIMD target.
        static const bool configured = [] {
            hwy::DisableTargets(HWY_AVX2);
            return true;
        }();
        (void)configured;
#endif
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

DecodedImage decode_jxl(const std::string& path) {
    DecodedImage img;

    configure_highway_targets();

    std::vector<uint8_t> data;
    if (!read_file(path, data)) {
        img.error = "JPEG XL 파일을 읽을 수 없습니다: " + path;
        return img;
    }

    // RAII 로 디코더/러너 정리
    auto dec = std::unique_ptr<JxlDecoderStruct, decltype(&JxlDecoderDestroy)>(
        JxlDecoderCreate(nullptr), JxlDecoderDestroy);
    if (!dec) {
        img.error = "JxlDecoderCreate 실패";
        return img;
    }
    auto runner = std::unique_ptr<void, decltype(&JxlThreadParallelRunnerDestroy)>(
        JxlThreadParallelRunnerCreate(nullptr,
                                      JxlThreadParallelRunnerDefaultNumWorkerThreads()),
        JxlThreadParallelRunnerDestroy);
    if (runner &&
        JxlDecoderSetParallelRunner(dec.get(), JxlThreadParallelRunner,
                                    runner.get()) != JXL_DEC_SUCCESS) {
        runner.reset();   // 러너 설정 실패 시 단일 스레드로 진행
    }

    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                             JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
        img.error = "JxlDecoderSubscribeEvents 실패";
        return img;
    }

    JxlDecoderSetInput(dec.get(), data.data(), data.size());
    JxlDecoderCloseInput(dec.get());

    const JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlBasicInfo info = {};
    bool haveInfo = false;
    ImageFrame frame;          // 현재 채우는 중인 프레임
    int pendingDelayMs = 0;    // FRAME 헤더에서 읽은 지연(ms)

    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());

        if (st == JXL_DEC_ERROR) {
            img.error = "JPEG XL 디코딩 오류: " + path;
            return img;
        }
        if (st == JXL_DEC_NEED_MORE_INPUT) {
            img.error = "JPEG XL 파일이 잘렸습니다(데이터 부족): " + path;
            return img;
        }
        if (st == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) {
                img.error = "JPEG XL 기본 정보를 읽지 못했습니다";
                return img;
            }
            if (info.xsize == 0 || info.ysize == 0) {
                img.error = "유효하지 않은 JPEG XL 크기입니다";
                return img;
            }
            img.width = (int)info.xsize;
            img.height = (int)info.ysize;
            haveInfo = true;
        } else if (st == JXL_DEC_FRAME) {
            JxlFrameHeader header = {};
            pendingDelayMs = 0;
            if (JxlDecoderGetFrameHeader(dec.get(), &header) == JXL_DEC_SUCCESS &&
                haveInfo && info.have_animation &&
                info.animation.tps_numerator > 0) {
                // duration 은 tick 단위 → ms 로 환산
                pendingDelayMs = (int)std::lround(
                    (double)header.duration * 1000.0 *
                    (double)info.animation.tps_denominator /
                    (double)info.animation.tps_numerator);
            }
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t bufSize = 0;
            if (JxlDecoderImageOutBufferSize(dec.get(), &format, &bufSize) !=
                    JXL_DEC_SUCCESS ||
                bufSize != (size_t)img.width * img.height * 4) {
                img.error = "JPEG XL 출력 버퍼 크기 계산 실패";
                return img;
            }
            frame.pixels.resize(bufSize);
            if (JxlDecoderSetImageOutBuffer(dec.get(), &format, frame.pixels.data(),
                                            bufSize) != JXL_DEC_SUCCESS) {
                img.error = "JPEG XL 출력 버퍼 설정 실패";
                return img;
            }
        } else if (st == JXL_DEC_FULL_IMAGE) {
            frame.delay_ms = pendingDelayMs;
            img.frames.push_back(std::move(frame));
            frame = ImageFrame{};
        } else if (st == JXL_DEC_SUCCESS) {
            break;
        } else {
            img.error = "예상치 못한 JPEG XL 디코더 상태입니다";
            return img;
        }
    }

    if (img.frames.empty()) {
        img.error = "JPEG XL 디코딩 실패(프레임 없음): " + path;
        return img;
    }

    img.pixels = img.frames.front().pixels;   // 첫 프레임(정적 표시/폴백용)
    img.ok = true;
    return img;
}
