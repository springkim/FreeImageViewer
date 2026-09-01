#pragma once
//
// heif_decoder.h
// libheif(libde265 백엔드)를 이용해 HEIF/HEIC 파일을 RGBA 픽셀로 디코딩한다.
//
#include "decoder/image_decoder.h"

// path 의 HEIF/HEIC 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_heif(const std::string &path, bool mt);
