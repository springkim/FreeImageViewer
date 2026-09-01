// stb_image 는 PSD의 레이어별 정보 대신 합성된 최종 이미지를 읽는다.
#include "decoder/psd_decoder.h"
#include "decoder/stb_decoder.h"

DecodedImage decode_psd(const std::string& path) {
    return decoder_detail::decode_stb_file(path, "PSD");
}
