#include "decoder/tga_decoder.h"
#include "decoder/stb_decoder.h"

DecodedImage decode_tga(const std::string& path) {
    return decoder_detail::decode_stb_file(path, "TGA");
}
