#pragma once
//
// heif_decoder.h
// libheif(libde265 백엔드)를 이용해 HEIF/HEIC 파일을 RGBA 픽셀로 디코딩한다.
//
#include "decoder/image_decoder.h"

// path 의 HEIF/HEIC 파일을 열어 RGBA 로 디코딩한다.
// mt=true 이면 libde265 HEVC worker와 HEIF grid tile 디코딩을 병렬화한다.
DecodedImage decode_heif(const std::string &path, bool mt);
