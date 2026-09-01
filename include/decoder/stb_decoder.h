#pragma once
//
// stb_decoder.h
// stb_image 공용 구현체. 포맷별 디코더가 이 함수를 통해 stb_image 를 사용한다.
//
#include "image_decoder.h"

#include <cstddef>

// 기존 BMP 디코더 진입점.
DecodedImage decode_stb(const std::string& path);

namespace decoder_detail {
    // stb_image 구현은 stb_decoder.cpp 한 곳에만 둔다.
    DecodedImage decode_stb_file(const std::string& path, const char* format_name);
    DecodedImage decode_stb_memory(const uint8_t* data, std::size_t size,
                                   const char* format_name);
}
