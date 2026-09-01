#pragma once
// stb_image 로 PSD의 합성(composited) 이미지를 RGBA8로 디코딩한다.
#include "image_decoder.h"

DecodedImage decode_psd(const std::string& path);
