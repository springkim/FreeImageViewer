#pragma once
//
// png_decoder.h
// libspng 를 이용해 PNG/APNG 파일을 RGBA 픽셀로 디코딩한다.
//
#include "image_decoder.h"

// path 의 PNG 파일을 열어 RGBA 로 디코딩한다. APNG는 여러 프레임으로 반환한다.
// mt=true 이면 APNG 프레임의 압축 해제를 병렬로 수행한다.
DecodedImage decode_png(const std::string& path, bool mt = false);
