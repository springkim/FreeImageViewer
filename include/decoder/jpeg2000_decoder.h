#pragma once
//
// jpeg2000_decoder.h
// OpenJPEG 를 이용해 JPEG 2000(JP2/J2K) 파일을 RGBA 픽셀로 디코딩한다.
//
#include "image_decoder.h"

// path 의 JP2 컨테이너 또는 원시 JPEG 2000 코드스트림을 RGBA 로 디코딩한다.
DecodedImage decode_jpeg2000(const std::string& path);
