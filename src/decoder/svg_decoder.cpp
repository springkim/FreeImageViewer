#include "decoder/svg_decoder.h"
#include "thread_count.h"

#include <resvg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

    // 시스템 폰트 검색은 매우 비싸므로 실제 text 요소가 있는 SVG에만 수행한다.
    // 검색에 실패하거나 SVGZ인 경우에는 렌더링 정확성을 위해 폰트를 로드한다.
    bool svg_may_use_text(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return true;
        }

        std::array<char, 8192> buffer{};
        std::string window;
        window.reserve(buffer.size() + 16);
        bool first_chunk = true;

        while (stream) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read_count = stream.gcount();
            if (read_count <= 0) {
                break;
            }

            if (first_chunk) {
                first_chunk = false;
                if (read_count >= 2 &&
                    static_cast<unsigned char>(buffer[0]) == 0x1f &&
                    static_cast<unsigned char>(buffer[1]) == 0x8b) {
                    return true;
                }
            }

            window.append(buffer.data(), static_cast<size_t>(read_count));
            if (window.find("<text") != std::string::npos ||
                window.find(":text") != std::string::npos) {
                return true;
            }

            // 태그가 버퍼 경계에 걸린 경우를 위해 접미부를 다음 검색에 남긴다.
            constexpr size_t kOverlap = 16;
            if (window.size() > kOverlap) {
                window.erase(0, window.size() - kOverlap);
            }
        }
        return false;
    }

    // resvg_options 안의 fontdb를 프로세스에서 한 번만 구축한다. options의
    // resources_dir 변경과 파싱을 함께 잠가 여러 디코드 스레드에서도 안전하다.
    class SystemFontOptions {
    public:
        ~SystemFontOptions() {
            if (options_) {
                resvg_options_destroy(options_);
            }
        }

        bool parse(const std::string& path, const std::string& resources_dir,
                   resvg_render_tree** tree, int32_t& error) {
            const std::lock_guard lock(mutex_);
            if (!options_) {
                options_ = resvg_options_create();
                if (options_) {
                    resvg_options_load_system_fonts(options_);
                }
            }
            if (!options_) {
                return false;
            }
            resvg_options_set_resources_dir(options_, resources_dir.c_str());
            error = resvg_parse_tree_from_file(path.c_str(), options_, tree);
            return true;
        }

        SystemFontOptions(const SystemFontOptions&) = delete;
        SystemFontOptions& operator=(const SystemFontOptions&) = delete;

        SystemFontOptions() = default;

    private:
        std::mutex mutex_;
        resvg_options* options_ = nullptr;
    };

    SystemFontOptions system_font_options;

    // resvg 출력은 premultiplied RGBA이므로 공용 DecodedImage의 straight RGBA로 바꾼다.
    void unpremultiply_range(std::vector<uint8_t>& pixels,
                             size_t begin_pixel, size_t end_pixel) {
        for (size_t pixel = begin_pixel; pixel < end_pixel; ++pixel) {
            const size_t i = pixel * 4;
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

    void unpremultiply_rgba(std::vector<uint8_t>& pixels, bool mt) {
        const size_t pixel_count = pixels.size() / 4;
        constexpr size_t kParallelThreshold = 2U * 1000 * 1000;
        const size_t thread_count = mt && pixel_count >= kParallelThreshold
            ? std::min<size_t>({static_cast<size_t>(decoder_detail::available_thread_count()),
                                pixel_count, 32})
            : 1;

        if (thread_count <= 1) {
            unpremultiply_range(pixels, 0, pixel_count);
            return;
        }

        std::vector<std::thread> workers;
        workers.reserve(thread_count - 1);
        const size_t chunk_size = (pixel_count + thread_count - 1) / thread_count;
        try {
            for (size_t i = 1; i < thread_count; ++i) {
                const size_t begin = std::min(i * chunk_size, pixel_count);
                const size_t end = std::min(begin + chunk_size, pixel_count);
                workers.emplace_back(unpremultiply_range, std::ref(pixels), begin, end);
            }
        } catch (...) {
            for (std::thread& worker : workers) {
                worker.join();
            }
            unpremultiply_range(pixels, 0, pixel_count);
            return;
        }
        unpremultiply_range(pixels, 0, std::min(chunk_size, pixel_count));
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
}

DecodedImage decode_svg(const std::string& path, bool mt) {
    DecodedImage image;

    const bool uses_text = svg_may_use_text(path);
    const std::string resources_dir = parent_directory(path);
    resvg_render_tree* tree = nullptr;
    int32_t error = RESVG_OK;

    if (uses_text) {
        if (!system_font_options.parse(path, resources_dir, &tree, error)) {
            image.error = "resvg 옵션 생성 실패";
            return image;
        }
    } else {
        resvg_options* options = resvg_options_create();
        if (!options) {
            image.error = "resvg 옵션 생성 실패";
            return image;
        }
        resvg_options_set_resources_dir(options, resources_dir.c_str());
        error = resvg_parse_tree_from_file(path.c_str(), options, &tree);
        resvg_options_destroy(options);
    }
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

    unpremultiply_rgba(image.pixels, mt);
    image.ok = true;
    return image;
}
