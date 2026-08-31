#pragma once
//
// jxl_decoder.h
// libjxl 을 이용해 JPEG XL 파일을 RGBA 픽셀로 디코딩한다(애니메이션 지원).
//
#include "image_decoder.h"

// path 의 JPEG XL 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_jxl(const std::string& path);
