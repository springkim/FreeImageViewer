#pragma once
//
// webp_decoder.h
// libwebp 를 이용해 WebP 파일을 RGBA 픽셀로 디코딩한다(애니메이션 지원).
//
#include "image_decoder.h"

// path 의 WebP 파일을 열어 RGBA 로 디코딩한다. Animated WebP는 여러 프레임으로 반환한다.
DecodedImage decode_webp(const std::string& path, bool mt);
