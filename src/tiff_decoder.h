#pragma once
//
// tiff_decoder.h
// libtiff 를 이용해 TIFF 파일을 RGBA 픽셀로 디코딩한다.
//
#include "image_decoder.h"

// path 의 TIFF 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_tiff(const std::string& path);
