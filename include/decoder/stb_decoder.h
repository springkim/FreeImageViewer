#pragma once
//
// stb_decoder.h
// stb_image 를 이용해 BMP/TGA 파일을 RGBA 픽셀로 디코딩한다.
//
#include "image_decoder.h"

// path 의 BMP 또는 TGA 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_stb(const std::string& path);
