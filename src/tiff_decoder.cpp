//
// tiff_decoder.cpp
// libtiff 의 고수준 API(TIFFReadRGBAImageOriented)로 TIFF 를 RGBA 로 디코딩한다.
// 다양한 색공간/비트깊이/압축을 라이브러리가 알아서 8비트 RGBA 로 변환해 준다.
//
#include "tiff_decoder.h"

#include <tiffio.h>

#include <vector>

DecodedImage decode_tiff(const std::string& path) {
    DecodedImage img;

    // 경미한 경고(비표준 태그 등)는 콘솔로 출력하지 않는다.
    TIFFSetWarningHandler(nullptr);

    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) {
        img.error = "TIFF 열기 실패(손상되었거나 지원하지 않는 TIFF): " + path;
        return img;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    if (width == 0 || height == 0) {
        img.error = "유효하지 않은 TIFF 크기입니다";
        TIFFClose(tif);
        return img;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint32_t> raster(pixelCount);

    // 좌상단 원점(TOPLEFT)으로 읽어 위→아래 스캔 순서를 맞춘다.
    if (!TIFFReadRGBAImageOriented(tif, width, height, raster.data(),
                                   ORIENTATION_TOPLEFT, /*stopOnError=*/0)) {
        img.error = "TIFF 디코딩 실패";
        TIFFClose(tif);
        return img;
    }
    TIFFClose(tif);

    // 패킹된 ABGR(uint32) → RGBA 바이트로 변환(엔디안 무관하게 매크로 사용).
    img.width  = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.pixels.resize(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t px = raster[i];
        uint8_t* out = &img.pixels[i * 4];
        out[0] = static_cast<uint8_t>(TIFFGetR(px));
        out[1] = static_cast<uint8_t>(TIFFGetG(px));
        out[2] = static_cast<uint8_t>(TIFFGetB(px));
        out[3] = static_cast<uint8_t>(TIFFGetA(px));
    }

    img.ok = true;
    return img;
}
