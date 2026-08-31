#pragma once
//
// png_decoder.h
// libspng 를 이용해 PNG 파일을 RGBA 픽셀로 디코딩한다.
//
#include "image_decoder.h"

// path 의 PNG 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_png(const std::string& path);
