#pragma once
//
// avif_decoder.h
// libavif(dav1d 백엔드)를 이용해 AVIF 파일을 RGBA 픽셀로 디코딩한다.
// 이미지 시퀀스(애니메이션 AVIF)는 여러 프레임으로 반환한다.
//
#include "decoder/image_decoder.h"

// path 의 AVIF 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_avif(const std::string& path);
