#pragma once
// stb_image 로 TGA 이미지를 RGBA8로 디코딩한다.
#include "image_decoder.h"

DecodedImage decode_tga(const std::string& path);
