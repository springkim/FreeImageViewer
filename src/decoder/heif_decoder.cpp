//
// heif_decoder.cpp
// libheif 로 HEIF/HEIC 를 8비트 RGBA 로 디코딩한다.
// 컨테이너 안의 대표 이미지(primary image)를 디코딩하며,
// 파일에 기록된 회전/미러/크롭 변환은 libheif 가 적용해 준다.
//
#include "decoder/heif_decoder.h"

#include <libheif/heif.h>

#include <cstring>

namespace {
    // libheif 는 최초 사용 전에 heif_init() 을 부르는 것이 권장된다(프로세스당 1회).
    void ensure_heif_init() {
        static const bool initialized = [] {
            heif_init(nullptr);
            return true;
        }();
        (void)initialized;
    }
} // namespace

DecodedImage decode_heif(const std::string& path) {
    DecodedImage img;
    ensure_heif_init();

    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        img.error = "heif_context_alloc 실패";
        return img;
    }

    heif_error err = heif_context_read_from_file(ctx, path.c_str(), nullptr);
    if (err.code != heif_error_Ok) {
        img.error = std::string("HEIF 열기 실패: ") + err.message;
        heif_context_free(ctx);
        return img;
    }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        img.error = std::string("HEIF 대표 이미지를 찾을 수 없습니다: ") + err.message;
        heif_context_free(ctx);
        return img;
    }

    // 8비트 인터리브 RGBA 로 디코딩(비트깊이/서브샘플링 변환은 라이브러리가 처리).
    heif_image* image = nullptr;
    err = heif_decode_image(handle, &image, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGBA, nullptr);
    if (err.code != heif_error_Ok || !image) {
        img.error = std::string("HEIF 디코딩 실패: ") + err.message;
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return img;
    }

    const int W = heif_image_get_width(image, heif_channel_interleaved);
    const int H = heif_image_get_height(image, heif_channel_interleaved);

    int stride = 0;
    const uint8_t* src = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
    if (W <= 0 || H <= 0 || !src || stride < W * 4) {
        img.error = "유효하지 않은 HEIF 픽셀 데이터입니다";
        heif_image_release(image);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return img;
    }

    // stride(행 패딩)를 고려해 한 행씩 복사.
    img.width  = W;
    img.height = H;
    img.pixels.resize(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        std::memcpy(&img.pixels[static_cast<size_t>(y) * W * 4],
                    src + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(W) * 4);
    }

    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);

    img.ok = true;
    return img;
}
