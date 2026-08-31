#pragma once
//
// exr_decoder.h
// OpenEXR 을 이용해 OpenEXR(.exr) 파일을 RGBA 픽셀로 디코딩한다.
// HDR(half float) 데이터는 화면 표시를 위해 8비트 sRGB 로 변환한다.
//
#include "decoder/image_decoder.h"

// path 의 EXR 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_exr(const std::string& path);
