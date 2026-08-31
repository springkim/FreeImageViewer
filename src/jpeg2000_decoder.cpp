//
// jpeg2000_decoder.cpp
// OpenJPEG 로 JP2/J2K 이미지를 디코딩하고 8비트 RGBA 로 변환한다.
//
#include "jpeg2000_decoder.h"

#if __has_include(<openjpeg.h>)
#include <openjpeg.h>
#elif __has_include(<openjpeg-2.5/openjpeg.h>)
#include <openjpeg-2.5/openjpeg.h>
#elif __has_include(<openjpeg-2.4/openjpeg.h>)
#include <openjpeg-2.4/openjpeg.h>
#else
#error "OpenJPEG header not found"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
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

    // 출력 픽셀의 reference-grid 좌표에 대응하는 component 샘플을 고른다.
    int32_t sample_at(const opj_image_comp_t& component,
                      uint32_t referenceX, uint32_t referenceY) {
        const uint64_t sampleX =
            (static_cast<uint64_t>(referenceX) + component.dx - 1) / component.dx;
        const uint64_t sampleY =
            (static_cast<uint64_t>(referenceY) + component.dy - 1) / component.dy;

        uint64_t x = sampleX > component.x0 ? sampleX - component.x0 : 0;
        uint64_t y = sampleY > component.y0 ? sampleY - component.y0 : 0;
        x = std::min<uint64_t>(x, component.w - 1);
        y = std::min<uint64_t>(y, component.h - 1);
        return component.data[y * component.w + x];
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

    uint8_t sample_u8(const opj_image_comp_t& component,
                      uint32_t referenceX, uint32_t referenceY) {
        return static_cast<uint8_t>(std::lround(
            sample_unit(component, sample_at(component, referenceX, referenceY)) * 255.0));
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

    std::string openjpeg_error(const std::string& operation,
                               const OpenJpegMessages& messages) {
        return messages.error.empty() ? operation : operation + ": " + messages.error;
    }
} // namespace

DecodedImage decode_jpeg2000(const std::string& path) {
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

    result.width = static_cast<int>(width64);
    result.height = static_cast<int>(height64);
    result.pixels.resize(static_cast<size_t>(width64 * height64 * 4));

    const opj_image_comp_t* alpha = alphaIndex >= 0 ? &image->comps[alphaIndex] : nullptr;
    const bool premultipliedAlpha = alpha && alpha->alpha == 2;
    for (uint32_t y = 0; y < height64; ++y) {
        const uint32_t referenceY = image->y0 + y;
        for (uint32_t x = 0; x < width64; ++x) {
            const uint32_t referenceX = image->x0 + x;
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;

            if (colorSpace == OPJ_CLRSPC_SYCC || colorSpace == OPJ_CLRSPC_EYCC) {
                const auto& yComp = image->comps[colorComponents[0]];
                const auto& cbComp = image->comps[colorComponents[1]];
                const auto& crComp = image->comps[colorComponents[2]];
                const double luminance = sample_unit(
                    yComp, sample_at(yComp, referenceX, referenceY)) * 255.0;
                const double cb = chroma_offset(
                    cbComp, sample_at(cbComp, referenceX, referenceY));
                const double cr = chroma_offset(
                    crComp, sample_at(crComp, referenceX, referenceY));
                red = clamp_u8(luminance + 1.402 * cr);
                green = clamp_u8(luminance - 0.344136 * cb - 0.714136 * cr);
                blue = clamp_u8(luminance + 1.772 * cb);
            } else if (colorSpace == OPJ_CLRSPC_CMYK) {
                const double cyan = sample_unit(image->comps[colorComponents[0]],
                    sample_at(image->comps[colorComponents[0]], referenceX, referenceY));
                const double magenta = sample_unit(image->comps[colorComponents[1]],
                    sample_at(image->comps[colorComponents[1]], referenceX, referenceY));
                const double yellow = sample_unit(image->comps[colorComponents[2]],
                    sample_at(image->comps[colorComponents[2]], referenceX, referenceY));
                const double black = sample_unit(image->comps[colorComponents[3]],
                    sample_at(image->comps[colorComponents[3]], referenceX, referenceY));
                red = clamp_u8((1.0 - cyan) * (1.0 - black) * 255.0);
                green = clamp_u8((1.0 - magenta) * (1.0 - black) * 255.0);
                blue = clamp_u8((1.0 - yellow) * (1.0 - black) * 255.0);
            } else if (colorSpace == OPJ_CLRSPC_GRAY || colorComponents.size() < 3) {
                red = green = blue = sample_u8(
                    image->comps[colorComponents[0]], referenceX, referenceY);
            } else {
                red = sample_u8(image->comps[colorComponents[0]], referenceX, referenceY);
                green = sample_u8(image->comps[colorComponents[1]], referenceX, referenceY);
                blue = sample_u8(image->comps[colorComponents[2]], referenceX, referenceY);
            }

            const uint8_t alphaValue = alpha
                ? sample_u8(*alpha, referenceX, referenceY)
                : uint8_t{255};
            if (premultipliedAlpha) {
                if (alphaValue == 0) {
                    red = green = blue = 0;
                } else {
                    red = clamp_u8(static_cast<double>(red) * 255.0 / alphaValue);
                    green = clamp_u8(static_cast<double>(green) * 255.0 / alphaValue);
                    blue = clamp_u8(static_cast<double>(blue) * 255.0 / alphaValue);
                }
            }

            const size_t offset = (static_cast<size_t>(y) * result.width + x) * 4;
            result.pixels[offset + 0] = red;
            result.pixels[offset + 1] = green;
            result.pixels[offset + 2] = blue;
            result.pixels[offset + 3] = alphaValue;
        }
    }

    result.ok = true;
    return result;
}
