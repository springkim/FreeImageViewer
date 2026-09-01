#pragma once
// ICO 컨테이너에서 가장 적합한 이미지를 골라 stb_image 로 RGBA8 디코딩한다.
#include "image_decoder.h"

DecodedImage decode_ico(const std::string& path);
