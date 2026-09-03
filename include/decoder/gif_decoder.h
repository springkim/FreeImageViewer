#pragma once
//
// gif_decoder.h
// giflib 를 이용해 GIF(정적/애니메이션)를 RGBA 프레임들로 디코딩한다.
//
#include "decoder/image_decoder.h"

// path 의 GIF 파일을 열어 RGBA 프레임 시퀀스로 디코딩한다.
// mt=true 이면 각 프레임의 독립적인 LZW 스트림을 병렬로 해제한다.
DecodedImage decode_gif(const std::string& path, bool mt = false);
