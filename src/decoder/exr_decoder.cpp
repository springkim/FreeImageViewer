//
// exr_decoder.cpp
// OpenEXR 의 고수준 API(RgbaInputFile)로 EXR 을 RGBA 로 디코딩한다.
// 채널 구성(RGB/RGBA/Y/YC)·타일/스캔라인·압축 방식은 라이브러리가 알아서 처리해 준다.
//
// EXR 은 선형(linear) HDR 부동소수 값을 담으므로 그대로 8비트로 자르면 어둡게 보인다.
// 여기서는 [0,1] 로 클램프한 뒤 sRGB 전달함수를 적용해 화면 표시용 8비트로 변환한다.
//
#include "decoder/exr_decoder.h"
#include "thread_count.h"

#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>
#include <Imath/ImathBox.h>
#include <Imath/half.h>

#include <cmath>
#include <exception>

namespace {
    // 선형 [0,1] → sRGB 8비트.
    uint8_t linear_to_srgb8(float v) {
        if (!(v > 0.0f)) {   // NaN 과 음수는 0 으로
            return 0;
        }
        if (v > 1.0f) {
            v = 1.0f;
        }
        const float s = (v <= 0.0031308f) ? (v * 12.92f)
                                          : (1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
        return static_cast<uint8_t>(std::lround(s * 255.0f));
    }

    // 알파는 색이 아니므로 감마 없이 [0,1] 클램프만 한다.
    uint8_t clamp_to_u8(float v) {
        if (!(v > 0.0f)) {
            return 0;
        }
        if (v > 1.0f) {
            v = 1.0f;
        }
        return static_cast<uint8_t>(std::lround(v * 255.0f));
    }

    // half 는 16비트뿐이므로 모든 비트 패턴에 대한 변환표를 미리 만들어 픽셀당 pow() 를 피한다.
    struct HalfLut {
        uint8_t color[65536];
        uint8_t alpha[65536];

        HalfLut() {
            for (int i = 0; i < 65536; ++i) {
                half h;
                h.setBits(static_cast<unsigned short>(i));
                const float f = static_cast<float>(h);
                color[i] = linear_to_srgb8(f);
                alpha[i] = clamp_to_u8(f);
            }
        }
    };

    const HalfLut& half_lut() {
        static const HalfLut lut;   // 최초 호출 시 1회만 생성
        return lut;
    }
} // namespace

DecodedImage decode_exr(const std::string& path, bool mt) {
    DecodedImage img;

    try {
        const int threadCount = mt ? decoder_detail::available_thread_count() : 0;
        Imf::RgbaInputFile file(path.c_str(), threadCount);

        const Imath::Box2i dw = file.dataWindow();
        const int64_t W = static_cast<int64_t>(dw.max.x) - dw.min.x + 1;
        const int64_t H = static_cast<int64_t>(dw.max.y) - dw.min.y + 1;
        if (W <= 0 || H <= 0) {
            img.error = "유효하지 않은 EXR 크기입니다";
            return img;
        }

        const size_t pixelCount = static_cast<size_t>(W) * static_cast<size_t>(H);
        Imf::Array2D<Imf::Rgba> raster(static_cast<long>(H), static_cast<long>(W));

        // 프레임버퍼 원점은 (0,0) 기준이므로 dataWindow 의 시작 오프셋만큼 base 를 당겨 준다.
        file.setFrameBuffer(&raster[0][0] - dw.min.x - dw.min.y * W, 1, W);
        file.readPixels(dw.min.y, dw.max.y);

        // half(선형) → 8비트 sRGB RGBA.
        const HalfLut& lut = half_lut();
        img.width  = static_cast<int>(W);
        img.height = static_cast<int>(H);
        img.pixels.resize(pixelCount * 4);
        const Imf::Rgba* src = &raster[0][0];
        for (size_t i = 0; i < pixelCount; ++i) {
            uint8_t* out = &img.pixels[i * 4];
            out[0] = lut.color[src[i].r.bits()];
            out[1] = lut.color[src[i].g.bits()];
            out[2] = lut.color[src[i].b.bits()];
            out[3] = lut.alpha[src[i].a.bits()];
        }

        img.ok = true;
        return img;
    } catch (const std::exception& e) {
        // OpenEXR 은 오류를 예외(Iex)로 던진다. 손상/미지원(deep 등) 파일이 여기로 온다.
        img.pixels.clear();
        img.ok = false;
        img.error = std::string("EXR 디코딩 실패: ") + e.what();
        return img;
    } catch (...) {
        img.pixels.clear();
        img.ok = false;
        img.error = "EXR 디코딩 실패(알 수 없는 오류): " + path;
        return img;
    }
}
