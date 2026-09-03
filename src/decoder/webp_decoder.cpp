//
// webp_decoder.cpp
// libwebp 로 정적 WebP와 Animated WebP를 RGBA 로 디코딩한다.
//
#include "decoder/webp_decoder.h"
#include "thread_count.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

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

    struct AnimatedWebPFrame {
        int xOffset = 0;
        int yOffset = 0;
        int width = 0;
        int height = 0;
        int duration = 0;
        bool hasAlpha = false;
        WebPMuxAnimDispose disposeMethod = WEBP_MUX_DISPOSE_NONE;
        WebPMuxAnimBlend blendMethod = WEBP_MUX_BLEND;
        const uint8_t* compressedData = nullptr;
        size_t compressedSize = 0;
    };

    struct DecodedWebPFrame {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
        std::string error;
    };

    bool read_animation_frames(const std::vector<uint8_t>& webp,
                               std::vector<AnimatedWebPFrame>& frames,
                               int& canvasWidth, int& canvasHeight,
                               std::unique_ptr<WebPDemuxer,
                                               decltype(&WebPDemuxDelete)>& demux,
                               std::string& error) {
        const WebPData data = {webp.data(), webp.size()};
        demux.reset(WebPDemux(&data));
        if (!demux) {
            error = "Animated WebP demux 실패";
            return false;
        }

        const uint32_t width = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
        const uint32_t height = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
        const uint32_t frameCount = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
        if (width == 0 || height == 0 || frameCount == 0 ||
            width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4) ||
            height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            frameCount > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            static_cast<size_t>(width) >
                std::numeric_limits<size_t>::max() / height / 4) {
            error = "유효하지 않은 Animated WebP 크기 또는 프레임 수입니다";
            return false;
        }

        canvasWidth = static_cast<int>(width);
        canvasHeight = static_cast<int>(height);
        frames.reserve(frameCount);
        for (uint32_t frameNumber = 1; frameNumber <= frameCount; ++frameNumber) {
            WebPIterator iterator{};
            if (!WebPDemuxGetFrame(demux.get(), static_cast<int>(frameNumber), &iterator)) {
                error = "Animated WebP 프레임 정보를 읽지 못했습니다";
                return false;
            }

            const bool valid = iterator.complete && iterator.x_offset >= 0 &&
                iterator.y_offset >= 0 && iterator.width > 0 && iterator.height > 0 &&
                iterator.width <= std::numeric_limits<int>::max() / 4 &&
                static_cast<int64_t>(iterator.x_offset) + iterator.width <= canvasWidth &&
                static_cast<int64_t>(iterator.y_offset) + iterator.height <= canvasHeight &&
                iterator.duration >= 0 && iterator.fragment.bytes != nullptr &&
                iterator.fragment.size > 0 &&
                (iterator.dispose_method == WEBP_MUX_DISPOSE_NONE ||
                 iterator.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND) &&
                (iterator.blend_method == WEBP_MUX_BLEND ||
                 iterator.blend_method == WEBP_MUX_NO_BLEND);
            if (!valid) {
                WebPDemuxReleaseIterator(&iterator);
                error = "올바르지 않은 Animated WebP 프레임 정보입니다";
                return false;
            }

            frames.push_back({iterator.x_offset, iterator.y_offset,
                              iterator.width, iterator.height, iterator.duration,
                              iterator.has_alpha != 0,
                              iterator.dispose_method, iterator.blend_method,
                              iterator.fragment.bytes, iterator.fragment.size});
            WebPDemuxReleaseIterator(&iterator);
        }
        return true;
    }

    DecodedWebPFrame decode_webp_frame(const AnimatedWebPFrame& frame) {
        DecodedWebPFrame decoded;
        decoded.width = frame.width;
        decoded.height = frame.height;

        try {
            const size_t pixelSize =
                static_cast<size_t>(frame.width) * frame.height * 4;
            decoded.pixels.resize(pixelSize);

            WebPDecoderConfig config{};
            if (!WebPInitDecoderConfig(&config)) {
                decoded.error = "Animated WebP 프레임 디코더 초기화 실패";
                return decoded;
            }
            config.output.colorspace = MODE_RGBA;
            config.output.is_external_memory = 1;
            config.output.u.RGBA.rgba = decoded.pixels.data();
            config.output.u.RGBA.stride = frame.width * 4;
            config.output.u.RGBA.size = decoded.pixels.size();
            // 여러 프레임을 동시에 풀기 때문에 코덱 내부 스레드는 끈다.
            config.options.use_threads = 0;

            const VP8StatusCode status =
                WebPDecode(frame.compressedData, frame.compressedSize, &config);
            if (status != VP8_STATUS_OK || config.output.width != frame.width ||
                config.output.height != frame.height) {
                decoded.error = "Animated WebP 프레임 압축 해제 실패";
            }
            WebPFreeDecBuffer(&config.output);
        } catch (const std::exception& exception) {
            decoded.error =
                std::string("Animated WebP 프레임 디코딩 실패: ") + exception.what();
        } catch (...) {
            decoded.error = "Animated WebP 프레임 디코딩 중 알 수 없는 오류가 발생했습니다";
        }
        return decoded;
    }

    void clear_frame_rect(std::vector<uint8_t>& canvas, int canvasWidth,
                          const AnimatedWebPFrame& frame) {
        const size_t rowSize = static_cast<size_t>(frame.width) * 4;
        for (int y = 0; y < frame.height; ++y) {
            const size_t offset =
                (static_cast<size_t>(frame.yOffset + y) * canvasWidth +
                 frame.xOffset) * 4;
            std::memset(canvas.data() + offset, 0, rowSize);
        }
    }

    void composite_webp_frame(std::vector<uint8_t>& canvas, int canvasWidth,
                              const AnimatedWebPFrame& frame,
                              const std::vector<uint8_t>& source,
                              bool sourceOnly) {
        const size_t rowSize = static_cast<size_t>(frame.width) * 4;
        for (int y = 0; y < frame.height; ++y) {
            const size_t sourceOffset = static_cast<size_t>(y) * rowSize;
            const size_t canvasOffset =
                (static_cast<size_t>(frame.yOffset + y) * canvasWidth +
                 frame.xOffset) * 4;
            uint8_t* destination = canvas.data() + canvasOffset;
            const uint8_t* sourceRow = source.data() + sourceOffset;

            if (sourceOnly || frame.blendMethod == WEBP_MUX_NO_BLEND) {
                std::memcpy(destination, sourceRow, rowSize);
                continue;
            }

            // WebPAnimDecoder의 non-premultiplied RGBA 정수 합성과 동일하다.
            for (int x = 0; x < frame.width; ++x) {
                const uint8_t* src = sourceRow + static_cast<size_t>(x) * 4;
                uint8_t* dst = destination + static_cast<size_t>(x) * 4;
                const uint32_t sourceAlpha = src[3];
                if (sourceAlpha == 0) {
                    continue;
                }
                if (sourceAlpha == 255) {
                    std::memcpy(dst, src, 4);
                    continue;
                }

                const uint32_t destinationFactorAlpha =
                    (static_cast<uint32_t>(dst[3]) * (256 - sourceAlpha)) >> 8;
                const uint32_t blendedAlpha = sourceAlpha + destinationFactorAlpha;
                const uint32_t scale = (1u << 24) / blendedAlpha;
                for (int channel = 0; channel < 3; ++channel) {
                    const uint32_t unscaled =
                        static_cast<uint32_t>(src[channel]) * sourceAlpha +
                        static_cast<uint32_t>(dst[channel]) * destinationFactorAlpha;
                    dst[channel] = static_cast<uint8_t>(
                        (static_cast<uint64_t>(unscaled) * scale) >> 24);
                }
                dst[3] = static_cast<uint8_t>(blendedAlpha);
            }
        }
    }

    bool decode_animation_parallel(const std::vector<uint8_t>& webp,
                                   DecodedImage& img) {
        using DemuxPtr =
            std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)>;
        DemuxPtr demux(nullptr, &WebPDemuxDelete);
        std::vector<AnimatedWebPFrame> frames;
        if (!read_animation_frames(webp, frames, img.width, img.height,
                                   demux, img.error)) {
            return false;
        }

        const size_t canvasSize = static_cast<size_t>(img.width) * img.height * 4;
        std::vector<uint8_t> canvas(canvasSize, 0);
        img.frames.reserve(frames.size());
        bool previousWasKeyFrame = false;

        auto compositeDecodedFrame = [&](size_t index, DecodedWebPFrame&& decoded) {
            const AnimatedWebPFrame& frame = frames[index];
            if (!decoded.error.empty() || decoded.width != frame.width ||
                decoded.height != frame.height ||
                decoded.pixels.size() !=
                    static_cast<size_t>(frame.width) * frame.height * 4) {
                img.error = decoded.error.empty()
                    ? "Animated WebP 프레임 크기가 올바르지 않습니다"
                    : decoded.error;
                return false;
            }

            const bool fullFrame = frame.width == img.width && frame.height == img.height;
            const bool keyFrame = index == 0 ||
                ((!frame.hasAlpha || frame.blendMethod == WEBP_MUX_NO_BLEND) && fullFrame) ||
                (frames[index - 1].disposeMethod == WEBP_MUX_DISPOSE_BACKGROUND &&
                 ((frames[index - 1].width == img.width &&
                   frames[index - 1].height == img.height) ||
                  previousWasKeyFrame));
            if (keyFrame) {
                std::fill(canvas.begin(), canvas.end(), 0);
            }
            composite_webp_frame(canvas, img.width, frame, decoded.pixels, keyFrame);
            ImageFrame output;
            output.pixels = canvas;
            output.delay_ms = frame.duration;
            img.frames.push_back(std::move(output));

            if (frame.disposeMethod == WEBP_MUX_DISPOSE_BACKGROUND) {
                clear_frame_rect(canvas, img.width, frame);
            }
            previousWasKeyFrame = keyFrame;
            return true;
        };

        size_t largestFrameSize = 0;
        for (const AnimatedWebPFrame& frame : frames) {
            largestFrameSize = std::max(
                largestFrameSize,
                static_cast<size_t>(frame.width) * frame.height * 4);
        }
        // 고해상도 프레임에서 작업자별 RGBA 버퍼가 과도하게 늘지 않도록
        // 동시에 압축 해제하는 임시 픽셀 버퍼를 약 256 MiB 이내로 제한한다.
        constexpr size_t parallelPixelBudget = 256u * 1024u * 1024u;
        const size_t memoryWorkerLimit =
            std::max<size_t>(1, parallelPixelBudget / largestFrameSize);
        const size_t requestedWorkerCount = std::min({
            static_cast<size_t>(decoder_detail::available_thread_count()),
            frames.size(), memoryWorkerLimit});
        if (requestedWorkerCount <= 1) {
            // 프레임 병렬화가 불가능한 경우 libwebp 자체 스레딩을 사용한다.
            return decode_animation(webp, true, img);
        } else {
            struct FrameSlot {
                size_t index = 0;
                int state = 0; // 0=empty, 1=decoding, 2=ready
                DecodedWebPFrame decoded;
            };
            std::vector<FrameSlot> slots(requestedWorkerCount);
            std::mutex slotsMutex;
            std::condition_variable slotsChanged;
            size_t nextToAssign = 0;
            bool cancelled = false;

            auto workerFunction = [&]() {
                for (;;) {
                    size_t index = 0;
                    {
                        std::unique_lock lock(slotsMutex);
                        slotsChanged.wait(lock, [&]() {
                            return cancelled || nextToAssign >= frames.size() ||
                                   slots[nextToAssign % slots.size()].state == 0;
                        });
                        if (cancelled || nextToAssign >= frames.size()) {
                            return;
                        }
                        index = nextToAssign++;
                        FrameSlot& slot = slots[index % slots.size()];
                        slot.index = index;
                        slot.state = 1;
                    }

                    DecodedWebPFrame decoded = decode_webp_frame(frames[index]);
                    {
                        std::lock_guard lock(slotsMutex);
                        FrameSlot& slot = slots[index % slots.size()];
                        slot.decoded = std::move(decoded);
                        slot.state = 2;
                    }
                    slotsChanged.notify_all();
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(requestedWorkerCount);
            for (size_t worker = 0; worker < requestedWorkerCount; ++worker) {
                try {
                    workers.emplace_back(workerFunction);
                } catch (const std::exception&) {
                    break;
                }
            }

            if (workers.empty()) {
                return decode_animation(webp, true, img);
            } else {
                bool compositionOk = true;
                try {
                    for (size_t index = 0; index < frames.size(); ++index) {
                        DecodedWebPFrame decoded;
                        {
                            std::unique_lock lock(slotsMutex);
                            FrameSlot& slot = slots[index % slots.size()];
                            slotsChanged.wait(lock, [&]() {
                                return slot.state == 2 && slot.index == index;
                            });
                            decoded = std::move(slot.decoded);
                            slot.state = 0;
                        }
                        slotsChanged.notify_all();
                        if (!compositeDecodedFrame(index, std::move(decoded))) {
                            compositionOk = false;
                            std::lock_guard lock(slotsMutex);
                            cancelled = true;
                            slotsChanged.notify_all();
                            break;
                        }
                    }
                } catch (...) {
                    {
                        std::lock_guard lock(slotsMutex);
                        cancelled = true;
                    }
                    slotsChanged.notify_all();
                    for (std::thread& worker : workers) {
                        worker.join();
                    }
                    throw;
                }
                for (std::thread& worker : workers) {
                    worker.join();
                }
                if (!compositionOk) {
                    img.frames.clear();
                    return false;
                }
            }
        }

        demux.reset();
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
        if (mt) {
            decode_animation_parallel(webp, img);
        } else {
            decode_animation(webp, false, img);
        }
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
