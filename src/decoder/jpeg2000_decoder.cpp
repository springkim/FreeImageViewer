//
// jpeg2000_decoder.cpp
// OpenJPEG 로 JP2/J2K 이미지를 디코딩하고 8비트 RGBA 로 변환한다.
//
#include "decoder/jpeg2000_decoder.h"

#if __has_include(<openjpeg.h>)
#include <openjpeg.h>
#elif __has_include(<openjpeg-2.5/openjpeg.h>)
#include <openjpeg-2.5/openjpeg.h>
#elif __has_include(<openjpeg-2.4/openjpeg.h>)
#include <openjpeg-2.4/openjpeg.h>
#else
#error "OpenJPEG header not found"
#endif

#include "thread_count.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
    using CodecPtr = std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)>;
    using StreamPtr = std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)>;
    using OpenJpegImagePtr = std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)>;

    struct OpenJpegMessages {
        std::string error;
    };

    void error_callback(const char* message, void* userData) {
        if (!message || !userData) {
            return;
        }
        auto& error = static_cast<OpenJpegMessages*>(userData)->error;
        if (!error.empty()) {
            error += ' ';
        }
        error += message;
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) {
            error.pop_back();
        }
    }

    bool detect_codec(const std::string& path, OPJ_CODEC_FORMAT& format) {
        std::array<uint8_t, 12> magic{};
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        const size_t size = std::fread(magic.data(), 1, magic.size(), file);
        std::fclose(file);

        static constexpr std::array<uint8_t, 12> jp2Signature = {
            0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ',
            0x0D, 0x0A, 0x87, 0x0A
        };
        if (size >= jp2Signature.size() && magic == jp2Signature) {
            format = OPJ_CODEC_JP2;
            return true;
        }

        // SOC(FF 4F) 다음에는 일반적으로 SIZ(FF 51) 마커가 온다.
        if (size >= 4 && magic[0] == 0xFF && magic[1] == 0x4F &&
            magic[2] == 0xFF && magic[3] == 0x51) {
            format = OPJ_CODEC_J2K;
            return true;
        }
        return false;
    }

    bool valid_component(const opj_image_comp_t& component) {
        return component.data && component.w > 0 && component.h > 0 &&
               component.dx > 0 && component.dy > 0 &&
               component.prec > 0 && component.prec <= 32;
    }

    double sample_unit(const opj_image_comp_t& component, int32_t value) {
        if (!component.sgnd) {
            if (component.prec == 32) {
                const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());
                return static_cast<double>(static_cast<uint32_t>(value)) / maximum;
            }
            const int64_t maximum = (int64_t{1} << component.prec) - 1;
            return static_cast<double>(std::clamp<int64_t>(value, 0, maximum)) /
                   static_cast<double>(maximum);
        }

        const int64_t minimum = component.prec == 32
            ? std::numeric_limits<int32_t>::min()
            : -(int64_t{1} << (component.prec - 1));
        const int64_t maximum = component.prec == 32
            ? std::numeric_limits<int32_t>::max()
            : (int64_t{1} << (component.prec - 1)) - 1;
        const int64_t clamped = std::clamp<int64_t>(value, minimum, maximum);
        return static_cast<double>(clamped - minimum) /
               static_cast<double>(maximum - minimum);
    }

    // YCbCr 계열의 Cb/Cr 값을 8비트 범위에 대응하는 signed 편차로 바꾼다.
    double chroma_offset(const opj_image_comp_t& component, int32_t value) {
        if (component.sgnd) {
            const double scale = component.prec == 32
                ? static_cast<double>(std::numeric_limits<uint32_t>::max())
                : static_cast<double>((uint64_t{1} << component.prec) - 1);
            return static_cast<double>(value) * 255.0 / scale;
        }

        const double maximum = component.prec == 32
            ? static_cast<double>(std::numeric_limits<uint32_t>::max())
            : static_cast<double>((uint64_t{1} << component.prec) - 1);
        const double midpoint = component.prec == 32
            ? 2147483648.0
            : static_cast<double>(uint64_t{1} << (component.prec - 1));
        const double unsignedValue = component.prec == 32
            ? static_cast<double>(static_cast<uint32_t>(value))
            : static_cast<double>(std::max<int32_t>(value, 0));
        return (unsignedValue - midpoint) * 255.0 / maximum;
    }

    uint8_t clamp_u8(double value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    }

    // component 하나에 대해 픽셀 변환에 필요한 표를 미리 만들어 둔다.
    //  * 출력 좌표 -> 샘플 좌표 변환은 나눗셈이라 픽셀마다 하면 비싸다.
    //    행/열 단위로 한 번만 계산해서 표로 들고 있는다.
    //  * 샘플 값 -> 8비트 변환도 정밀도가 16비트 이하면 룩업 테이블로 대체한다.
    //    (실제 파일은 대부분 8/12/16비트라 이 경로를 탄다)
    struct ComponentTable {
        const int32_t* data = nullptr;
        const opj_image_comp_t* component = nullptr;
        std::vector<uint32_t> columnIndex;  // 출력 x -> component 안의 x
        std::vector<size_t> rowOffset;      // 출력 y -> component 안의 행 시작 오프셋
        std::vector<uint8_t> toU8;          // 샘플 값 -> 8비트 밝기
        std::vector<double> toUnit;         // 샘플 값 -> 0..1 정규화 값
        std::vector<double> toChroma;       // 샘플 값 -> 8비트 색차 편차
        int32_t minimum = 0;
        int32_t maximum = 0;

        int32_t raw(size_t x, size_t y) const {
            return data[rowOffset[y] + columnIndex[x]];
        }

        size_t lutIndex(int32_t value) const {
            return static_cast<size_t>(std::clamp(value, minimum, maximum) - minimum);
        }

        uint8_t u8(size_t x, size_t y) const {
            const int32_t value = raw(x, y);
            if (!toU8.empty()) {
                return toU8[lutIndex(value)];
            }
            return static_cast<uint8_t>(
                std::lround(sample_unit(*component, value) * 255.0));
        }

        double unit(size_t x, size_t y) const {
            const int32_t value = raw(x, y);
            if (!toUnit.empty()) {
                return toUnit[lutIndex(value)];
            }
            return sample_unit(*component, value);
        }

        double chroma(size_t x, size_t y) const {
            const int32_t value = raw(x, y);
            if (!toChroma.empty()) {
                return toChroma[lutIndex(value)];
            }
            return chroma_offset(*component, value);
        }
    };

    // 어떤 룩업 테이블이 필요한지.
    enum class TableKind { U8, Unit, Chroma };

    ComponentTable build_table(const opj_image_comp_t& component,
                               uint32_t originX, uint32_t originY,
                               uint32_t width, uint32_t height,
                               TableKind kind) {
        ComponentTable table;
        table.data = component.data;
        table.component = &component;

        table.columnIndex.resize(width);
        for (uint32_t x = 0; x < width; ++x) {
            const uint64_t referenceX = static_cast<uint64_t>(originX) + x;
            const uint64_t sampleX = (referenceX + component.dx - 1) / component.dx;
            const uint64_t index = sampleX > component.x0 ? sampleX - component.x0 : 0;
            table.columnIndex[x] =
                static_cast<uint32_t>(std::min<uint64_t>(index, component.w - 1));
        }

        table.rowOffset.resize(height);
        for (uint32_t y = 0; y < height; ++y) {
            const uint64_t referenceY = static_cast<uint64_t>(originY) + y;
            const uint64_t sampleY = (referenceY + component.dy - 1) / component.dy;
            const uint64_t index = sampleY > component.y0 ? sampleY - component.y0 : 0;
            table.rowOffset[y] =
                static_cast<size_t>(std::min<uint64_t>(index, component.h - 1)) *
                component.w;
        }

        if (component.prec <= 16) {
            const size_t entries = size_t{1} << component.prec;
            if (component.sgnd) {
                table.minimum = -(int32_t{1} << (component.prec - 1));
                table.maximum = (int32_t{1} << (component.prec - 1)) - 1;
            } else {
                table.minimum = 0;
                table.maximum = static_cast<int32_t>(entries - 1);
            }
            switch (kind) {
                case TableKind::U8:     table.toU8.resize(entries); break;
                case TableKind::Unit:   table.toUnit.resize(entries); break;
                case TableKind::Chroma: table.toChroma.resize(entries); break;
            }
            for (size_t i = 0; i < entries; ++i) {
                const int32_t value = static_cast<int32_t>(i) + table.minimum;
                switch (kind) {
                    case TableKind::U8:
                        table.toU8[i] = static_cast<uint8_t>(
                            std::lround(sample_unit(component, value) * 255.0));
                        break;
                    case TableKind::Unit:
                        table.toUnit[i] = sample_unit(component, value);
                        break;
                    case TableKind::Chroma:
                        table.toChroma[i] = chroma_offset(component, value);
                        break;
                }
            }
        }
        return table;
    }

    // 서브샘플링이 없고 8비트 unsigned 인 흔한 경우에는 표를 거치지 않고
    // 원본 샘플을 그대로 복사할 수 있다.
    bool is_direct_u8(const ComponentTable& table, uint32_t width, uint32_t height) {
        return table.component->prec == 8 && !table.component->sgnd &&
               table.component->dx == 1 && table.component->dy == 1 &&
               table.component->w >= width && table.component->h >= height;
    }

    std::string openjpeg_error(const std::string& operation,
                               const OpenJpegMessages& messages) {
        return messages.error.empty() ? operation : operation + ": " + messages.error;
    }

    // 행 구간을 워커 스레드로 나눠서 실행한다.
    void run_by_rows(uint32_t width, uint32_t height, bool mt,
                     const std::function<void(uint32_t, uint32_t)>& body) {
        // 스레드 하나가 최소 이만큼은 맡아야 생성 비용이 아깝지 않다.
        constexpr uint64_t kMinPixelsPerWorker = 128u * 1024u;
        uint32_t workerCount = 1;
        if (mt) {
            const uint64_t pixels = static_cast<uint64_t>(width) * height;
            workerCount = static_cast<uint32_t>(std::max<uint64_t>(
                std::min<uint64_t>({pixels / kMinPixelsPerWorker, height,
                                    static_cast<uint64_t>(
                                        decoder_detail::available_thread_count())}),
                1));
        }
        if (workerCount <= 1) {
            body(0, height);
            return;
        }

        const uint32_t chunk = (height + workerCount - 1) / workerCount;
        std::vector<std::thread> workers;
        workers.reserve(workerCount - 1);
        for (uint32_t worker = 1; worker < workerCount; ++worker) {
            const uint32_t begin = std::min(worker * chunk, height);
            const uint32_t end = std::min(begin + chunk, height);
            if (begin < end) {
                workers.emplace_back(body, begin, end);
            }
        }
        body(0, std::min(chunk, height));
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
} // namespace

DecodedImage decode_jpeg2000(const std::string& path, bool mt) {
    DecodedImage result;

    OPJ_CODEC_FORMAT format = OPJ_CODEC_UNKNOWN;
    if (!detect_codec(path, format)) {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            result.error = "파일을 열 수 없습니다: " + path;
        } else {
            std::fclose(file);
            result.error = "유효한 JPEG 2000(JP2/J2K) 파일이 아닙니다: " + path;
        }
        return result;
    }

    StreamPtr stream(opj_stream_create_default_file_stream(path.c_str(), OPJ_TRUE),
                     opj_stream_destroy);
    if (!stream) {
        result.error = "JPEG 2000 파일 스트림을 열 수 없습니다: " + path;
        return result;
    }

    CodecPtr codec(opj_create_decompress(format), opj_destroy_codec);
    if (!codec) {
        result.error = "OpenJPEG 디코더 생성 실패";
        return result;
    }

    OpenJpegMessages messages;
    opj_set_info_handler(codec.get(), nullptr, nullptr);
    opj_set_warning_handler(codec.get(), nullptr, nullptr);
    opj_set_error_handler(codec.get(), error_callback, &messages);

    opj_dparameters_t parameters{};
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec.get(), &parameters)) {
        result.error = openjpeg_error("OpenJPEG 디코더 설정 실패", messages);
        return result;
    }

    // Calling this even for the single-thread case overrides OPJ_NUM_THREADS,
    // so mt=false always decodes on the calling thread.
    if (opj_has_thread_support()) {
        const int threadCount = mt ? std::max(opj_get_num_cpus(), 1) : 0;
        opj_codec_set_threads(codec.get(), threadCount);
    }

    opj_image_t* rawImage = nullptr;
    if (!opj_read_header(stream.get(), codec.get(), &rawImage) || !rawImage) {
        result.error = openjpeg_error("JPEG 2000 헤더 파싱 실패", messages);
        return result;
    }
    OpenJpegImagePtr image(rawImage, opj_image_destroy);

    if (image->x1 <= image->x0 || image->y1 <= image->y0 || image->numcomps == 0 ||
        !image->comps) {
        result.error = "유효하지 않은 JPEG 2000 이미지 정보입니다";
        return result;
    }

    const uint64_t width64 = static_cast<uint64_t>(image->x1) - image->x0;
    const uint64_t height64 = static_cast<uint64_t>(image->y1) - image->y0;
    if (width64 > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        height64 > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        width64 > std::numeric_limits<size_t>::max() / 4 / height64) {
        result.error = "JPEG 2000 이미지가 너무 큽니다";
        return result;
    }

    if (!opj_decode(codec.get(), stream.get(), image.get()) ||
        !opj_end_decompress(codec.get(), stream.get())) {
        result.error = openjpeg_error("JPEG 2000 디코딩 실패", messages);
        return result;
    }

    for (uint32_t i = 0; i < image->numcomps; ++i) {
        if (!valid_component(image->comps[i])) {
            result.error = "JPEG 2000 구성 요소 데이터가 유효하지 않습니다";
            return result;
        }
    }

    int alphaIndex = -1;
    for (uint32_t i = 0; i < image->numcomps; ++i) {
        if (image->comps[i].alpha == 1 || image->comps[i].alpha == 2) {
            alphaIndex = static_cast<int>(i);
            break;
        }
    }
    if (alphaIndex < 0) {
        if (image->numcomps == 2) {
            alphaIndex = 1; // 원시 코드스트림에는 JP2 cdef가 없으므로 관례를 적용한다.
        } else if (image->numcomps == 4 && image->color_space != OPJ_CLRSPC_CMYK) {
            alphaIndex = 3;
        } else if (image->numcomps == 5 && image->color_space == OPJ_CLRSPC_CMYK) {
            alphaIndex = 4;
        }
    }

    std::vector<uint32_t> colorComponents;
    colorComponents.reserve(image->numcomps);
    for (uint32_t i = 0; i < image->numcomps; ++i) {
        if (static_cast<int>(i) != alphaIndex) {
            colorComponents.push_back(i);
        }
    }
    if (colorComponents.empty()) {
        result.error = "JPEG 2000 색상 구성 요소가 없습니다";
        return result;
    }

    OPJ_COLOR_SPACE colorSpace = image->color_space;
    if (colorSpace != OPJ_CLRSPC_SYCC && colorSpace != OPJ_CLRSPC_EYCC &&
        colorSpace != OPJ_CLRSPC_CMYK && colorComponents.size() >= 3) {
        const auto& first = image->comps[colorComponents[0]];
        const auto& second = image->comps[colorComponents[1]];
        const auto& third = image->comps[colorComponents[2]];
        if (second.dx != first.dx || second.dy != first.dy ||
            third.dx != first.dx || third.dy != first.dy) {
            colorSpace = OPJ_CLRSPC_SYCC;
        }
    }

    if ((colorSpace == OPJ_CLRSPC_SYCC || colorSpace == OPJ_CLRSPC_EYCC) &&
        colorComponents.size() < 3) {
        result.error = "JPEG 2000 YCbCr 구성 요소가 부족합니다";
        return result;
    }
    if (colorSpace == OPJ_CLRSPC_CMYK && colorComponents.size() < 4) {
        result.error = "JPEG 2000 CMYK 구성 요소가 부족합니다";
        return result;
    }

    const uint32_t width = static_cast<uint32_t>(width64);
    const uint32_t height = static_cast<uint32_t>(height64);
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.pixels.resize(static_cast<size_t>(width64 * height64 * 4));

    const bool ycc = colorSpace == OPJ_CLRSPC_SYCC || colorSpace == OPJ_CLRSPC_EYCC;
    const bool cmyk = colorSpace == OPJ_CLRSPC_CMYK;
    const bool gray = !ycc && !cmyk &&
                      (colorSpace == OPJ_CLRSPC_GRAY || colorComponents.size() < 3);
    const size_t colorTableCount = ycc ? 3 : (cmyk ? 4 : (gray ? 1 : 3));

    std::vector<ComponentTable> color;
    color.reserve(colorTableCount);
    for (size_t i = 0; i < colorTableCount; ++i) {
        // YCbCr 은 Y 를 반올림 없이 써야 하고 Cb/Cr 은 색차 편차가 필요하다.
        // CMYK 는 네 성분 모두 0..1 정규화 값으로 섞는다.
        TableKind kind = TableKind::U8;
        if (ycc) {
            kind = i == 0 ? TableKind::Unit : TableKind::Chroma;
        } else if (cmyk) {
            kind = TableKind::Unit;
        }
        color.push_back(build_table(image->comps[colorComponents[i]],
                                    image->x0, image->y0, width, height, kind));
    }

    ComponentTable alphaTable;
    const bool hasAlpha = alphaIndex >= 0;
    if (hasAlpha) {
        alphaTable = build_table(image->comps[alphaIndex], image->x0, image->y0,
                                 width, height, TableKind::U8);
    }
    const bool premultipliedAlpha = hasAlpha && image->comps[alphaIndex].alpha == 2;

    // 서브샘플링/비트심도 변환이 필요 없는 경우의 빠른 경로.
    const bool directRgb = !ycc && !cmyk && !gray && !premultipliedAlpha &&
                           is_direct_u8(color[0], width, height) &&
                           is_direct_u8(color[1], width, height) &&
                           is_direct_u8(color[2], width, height) &&
                           (!hasAlpha || is_direct_u8(alphaTable, width, height));
    const bool directGray = gray && !premultipliedAlpha &&
                            is_direct_u8(color[0], width, height) &&
                            (!hasAlpha || is_direct_u8(alphaTable, width, height));

    auto convertRows = [&](uint32_t beginY, uint32_t endY) {
        for (uint32_t y = beginY; y < endY; ++y) {
            uint8_t* out = result.pixels.data() + static_cast<size_t>(y) * width * 4;

            if (directRgb) {
                const int32_t* r = color[0].data + color[0].rowOffset[y];
                const int32_t* g = color[1].data + color[1].rowOffset[y];
                const int32_t* b = color[2].data + color[2].rowOffset[y];
                const int32_t* a = hasAlpha
                    ? alphaTable.data + alphaTable.rowOffset[y]
                    : nullptr;
                for (uint32_t x = 0; x < width; ++x, out += 4) {
                    out[0] = static_cast<uint8_t>(std::clamp(r[x], 0, 255));
                    out[1] = static_cast<uint8_t>(std::clamp(g[x], 0, 255));
                    out[2] = static_cast<uint8_t>(std::clamp(b[x], 0, 255));
                    out[3] = a ? static_cast<uint8_t>(std::clamp(a[x], 0, 255))
                               : uint8_t{255};
                }
                continue;
            }

            if (directGray) {
                const int32_t* v = color[0].data + color[0].rowOffset[y];
                const int32_t* a = hasAlpha
                    ? alphaTable.data + alphaTable.rowOffset[y]
                    : nullptr;
                for (uint32_t x = 0; x < width; ++x, out += 4) {
                    const uint8_t value = static_cast<uint8_t>(std::clamp(v[x], 0, 255));
                    out[0] = value;
                    out[1] = value;
                    out[2] = value;
                    out[3] = a ? static_cast<uint8_t>(std::clamp(a[x], 0, 255))
                               : uint8_t{255};
                }
                continue;
            }

            for (uint32_t x = 0; x < width; ++x, out += 4) {
                uint8_t red = 0;
                uint8_t green = 0;
                uint8_t blue = 0;

                if (ycc) {
                    const double luminance = color[0].unit(x, y) * 255.0;
                    const double cb = color[1].chroma(x, y);
                    const double cr = color[2].chroma(x, y);
                    red = clamp_u8(luminance + 1.402 * cr);
                    green = clamp_u8(luminance - 0.344136 * cb - 0.714136 * cr);
                    blue = clamp_u8(luminance + 1.772 * cb);
                } else if (cmyk) {
                    const double cyan = color[0].unit(x, y);
                    const double magenta = color[1].unit(x, y);
                    const double yellow = color[2].unit(x, y);
                    const double black = color[3].unit(x, y);
                    red = clamp_u8((1.0 - cyan) * (1.0 - black) * 255.0);
                    green = clamp_u8((1.0 - magenta) * (1.0 - black) * 255.0);
                    blue = clamp_u8((1.0 - yellow) * (1.0 - black) * 255.0);
                } else if (gray) {
                    red = green = blue = color[0].u8(x, y);
                } else {
                    red = color[0].u8(x, y);
                    green = color[1].u8(x, y);
                    blue = color[2].u8(x, y);
                }

                const uint8_t alphaValue = hasAlpha ? alphaTable.u8(x, y) : uint8_t{255};
                if (premultipliedAlpha) {
                    if (alphaValue == 0) {
                        red = green = blue = 0;
                    } else {
                        const unsigned divisor = alphaValue;
                        red = static_cast<uint8_t>(std::min(
                            (red * 255U + divisor / 2U) / divisor, 255U));
                        green = static_cast<uint8_t>(std::min(
                            (green * 255U + divisor / 2U) / divisor, 255U));
                        blue = static_cast<uint8_t>(std::min(
                            (blue * 255U + divisor / 2U) / divisor, 255U));
                    }
                }

                out[0] = red;
                out[1] = green;
                out[2] = blue;
                out[3] = alphaValue;
            }
        }
    };

    run_by_rows(width, height, mt, convertRows);

    result.ok = true;
    return result;
}
