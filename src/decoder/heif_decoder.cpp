//
// heif_decoder.cpp
// libheif 로 HEIF/HEIC 를 8비트 RGBA 로 디코딩한다.
// 컨테이너 안의 대표 이미지(primary image)를 디코딩하며,
// 파일에 기록된 회전/미러/크롭 변환은 libheif 가 적용해 준다.
//
#include "decoder/heif_decoder.h"
#include "thread_count.h"

#include <libheif/heif.h>
#include <libheif/heif_plugin.h>
#include <libde265/de265.h>

#include <algorithm>
#include <cstring>

// 번들 libheif 정적 라이브러리의 built-in libde265 플러그인 접근자.
extern const heif_decoder_plugin* get_decoder_plugin_libde265();

namespace {
    // 번들된 libheif 1.19의 libde265 플러그인은 디코더마다 worker를 1개만
    // 시작한다. libheif의 max_decoding_threads 설정은 grid tile에만 적용되므로,
    // 일반 HEIF도 병렬 디코딩되도록 동일 플러그인의 생성 함수만 감싼다.
    // libheif 1.19.8 decoder_libde265.cc 구조체의 첫 필드. 나머지 필드는
    // 원본 플러그인에서 관리하며, 여기서는 decoder context만 교체한다.
    struct Libde265DecoderPrefix {
        de265_decoder_context* context;
    };

    const heif_decoder_plugin* libde265_plugin = nullptr;
    heif_decoder_plugin threaded_libde265_plugin{};
    thread_local int requested_heif_workers = 1;

    heif_error new_threaded_libde265_decoder(void** decoder_raw) {
        const heif_error err = libde265_plugin->new_decoder(decoder_raw);
        if (err.code != heif_error_Ok || !decoder_raw || !*decoder_raw ||
            requested_heif_workers <= 1) {
            return err;
        }

        auto* decoder = static_cast<Libde265DecoderPrefix*>(*decoder_raw);
        de265_decoder_context* replacement = de265_new_decoder();
        if (!replacement) {
            // 메모리가 부족해 추가 worker를 만들 수 없으면 원래 단일 worker로
            // 계속 디코딩한다.
            return err;
        }

        const int worker_count = std::min(requested_heif_workers, 32);
        const de265_error thread_err =
            de265_start_worker_threads(replacement, worker_count);
        if (!de265_isOK(thread_err)) {
            de265_free_decoder(replacement);
            return err;
        }

        de265_free_decoder(decoder->context);
        decoder->context = replacement;
        return err;
    }

    int threaded_libde265_priority(heif_compression_format format) {
        return format == heif_compression_HEVC ? 200 : 0;
    }

    // libheif 는 최초 사용 전에 heif_init() 을 부르는 것이 권장된다(프로세스당 1회).
    void ensure_heif_init() {
        static const bool initialized = [] {
            heif_init(nullptr);

            libde265_plugin = get_decoder_plugin_libde265();
            if (libde265_plugin) {
                threaded_libde265_plugin = *libde265_plugin;
                threaded_libde265_plugin.init_plugin = nullptr;
                threaded_libde265_plugin.deinit_plugin = nullptr;
                threaded_libde265_plugin.does_support_format =
                    threaded_libde265_priority;
                threaded_libde265_plugin.new_decoder =
                    new_threaded_libde265_decoder;
                threaded_libde265_plugin.id_name = "libde265-threaded";
                heif_register_decoder_plugin(&threaded_libde265_plugin);
            }
            return true;
        }();
        (void)initialized;
    }

    class ScopedHeifWorkerCount {
    public:
        explicit ScopedHeifWorkerCount(int count)
            : previous_(requested_heif_workers) {
            requested_heif_workers = count;
        }

        ~ScopedHeifWorkerCount() {
            requested_heif_workers = previous_;
        }

        ScopedHeifWorkerCount(const ScopedHeifWorkerCount&) = delete;
        ScopedHeifWorkerCount& operator=(const ScopedHeifWorkerCount&) = delete;

    private:
        int previous_;
    };
} // namespace

DecodedImage decode_heif(const std::string& path, bool mt) {
    DecodedImage img;
    ensure_heif_init();

    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        img.error = "heif_context_alloc 실패";
        return img;
    }
    heif_context_set_max_decoding_threads(
        ctx, mt ? decoder_detail::available_thread_count() : 0);

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
    {
        const ScopedHeifWorkerCount worker_count(
            mt ? decoder_detail::available_thread_count() : 1);
        err = heif_decode_image(handle, &image, heif_colorspace_RGB,
                                heif_chroma_interleaved_RGBA, nullptr);
    }
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
