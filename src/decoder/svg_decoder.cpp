#include "decoder/svg_decoder.h"

#include <resvg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {
    constexpr uint64_t kMaxPixels = 400ULL * 1000 * 1000;

    const char* resvg_error_message(int32_t error) {
        switch (error) {
            case RESVG_ERROR_NOT_AN_UTF8_STR: return "경로나 SVG가 UTF-8이 아님";
            case RESVG_ERROR_FILE_OPEN_FAILED: return "파일 열기 실패";
            case RESVG_ERROR_MALFORMED_GZIP: return "잘못된 SVGZ 압축";
            case RESVG_ERROR_ELEMENTS_LIMIT_REACHED: return "SVG 요소 수 제한 초과";
            case RESVG_ERROR_INVALID_SIZE: return "유효하지 않은 SVG 크기";
            case RESVG_ERROR_PARSING_FAILED: return "SVG 구문 분석 실패";
            default: return "알 수 없는 오류";
        }
    }

    std::string parent_directory(const std::string& path) {
        const size_t separator = path.find_last_of("/\\");
        if (separator == std::string::npos) {
            return ".";
        }
        if (separator == 0) {
            return path.substr(0, 1);
        }
        // "C:\\file.svg"의 부모는 "C:\\"이다. "C:"는 드라이브의 현재 폴더를 뜻한다.
        if (separator == 2 && path.size() >= 3 && path[1] == ':') {
            return path.substr(0, 3);
        }
        return path.substr(0, separator);
    }

    // resvg 출력은 premultiplied RGBA이므로 공용 DecodedImage의 straight RGBA로 바꾼다.
    void unpremultiply_rgba(std::vector<uint8_t>& pixels) {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned alpha = pixels[i + 3];
            if (alpha == 0) {
                pixels[i] = pixels[i + 1] = pixels[i + 2] = 0;
                continue;
            }
            if (alpha == 255) {
                continue;
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                const unsigned value =
                    (static_cast<unsigned>(pixels[i + channel]) * 255U + alpha / 2U) / alpha;
                pixels[i + channel] = static_cast<uint8_t>(std::min(value, 255U));
            }
        }
    }
}

DecodedImage decode_svg(const std::string& path) {
    DecodedImage image;

    resvg_options* options = resvg_options_create();
    if (!options) {
        image.error = "resvg 옵션 생성 실패";
        return image;
    }
    const std::string resources_dir = parent_directory(path);
    resvg_options_set_resources_dir(options, resources_dir.c_str());
    resvg_options_load_system_fonts(options);

    resvg_render_tree* tree = nullptr;
    const int32_t error = resvg_parse_tree_from_file(path.c_str(), options, &tree);
    resvg_options_destroy(options);
    if (error != RESVG_OK || !tree) {
        image.error = std::string("SVG 디코딩 실패(") + resvg_error_message(error) +
                      "): " + path;
        return image;
    }

    const resvg_size svg_size = resvg_get_image_size(tree);
    if (!std::isfinite(svg_size.width) || !std::isfinite(svg_size.height) ||
        svg_size.width <= 0.0f || svg_size.height <= 0.0f ||
        svg_size.width > static_cast<float>(std::numeric_limits<int>::max()) ||
        svg_size.height > static_cast<float>(std::numeric_limits<int>::max())) {
        resvg_tree_destroy(tree);
        image.error = "유효하지 않은 SVG 크기입니다: " + path;
        return image;
    }

    const uint32_t width = static_cast<uint32_t>(std::ceil(svg_size.width));
    const uint32_t height = static_cast<uint32_t>(std::ceil(svg_size.height));
    const uint64_t pixel_count = static_cast<uint64_t>(width) * height;
    if (width == 0 || height == 0 || pixel_count > kMaxPixels ||
        pixel_count > std::numeric_limits<size_t>::max() / 4) {
        resvg_tree_destroy(tree);
        image.error = "SVG 이미지가 너무 큽니다: " + path;
        return image;
    }

    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.pixels.assign(static_cast<size_t>(pixel_count) * 4, 0);
    resvg_render(tree, resvg_transform_identity(), width, height,
                 reinterpret_cast<char*>(image.pixels.data()));
    resvg_tree_destroy(tree);

    unpremultiply_rgba(image.pixels);
    image.ok = true;
    return image;
}
