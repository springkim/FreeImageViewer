#pragma once
// resvg 로 SVG를 래스터화해 RGBA8로 디코딩한다.
#include "image_decoder.h"

DecodedImage decode_svg(const std::string& path, bool mt = false);
