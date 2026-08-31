#pragma once
//
// qoi_decoder.h
// QOI(Quite OK Image) 파일을 RGBA 픽셀로 디코딩한다(자체 구현, 외부 라이브러리 불필요).
//
#include "image_decoder.h"

// path 의 QOI 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_qoi(const std::string& path);
