#pragma once
//
// pnm_decoder.h
// PNM 계열(PBM/PGM/PPM, P1~P6)을 RGBA 픽셀로 디코딩한다(자체 구현).
//
#include "image_decoder.h"

// path 의 PNM(P1~P6) 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_pnm(const std::string& path);
