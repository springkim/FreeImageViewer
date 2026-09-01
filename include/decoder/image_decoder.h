#pragma once
//
// image_decoder.h
// 여러 이미지 포맷(JPEG/PNG/WebP/GIF)을 RGBA 픽셀로 디코딩하는 공용 인터페이스.
// 실제 디코딩은 포맷별 구현(jpeg/png/webp/gif_decoder)에 위임한다.
//
#include <string>
#include <vector>
#include <cstdint>

// 애니메이션 한 프레임(합성 완료된 전체 캔버스).
struct ImageFrame {
    std::vector<uint8_t> pixels; // top-down, 8-bit RGBA, 크기 = width * height * 4
    int delay_ms = 0; // 이 프레임을 표시할 시간(ms)
};

// 디코딩 결과. ok == true 일 때만 유효.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // 단일(또는 첫) 프레임 RGBA (width*height*4)
    std::vector<ImageFrame> frames; // 애니메이션 프레임들(정적 이미지는 비어 있음)
    bool ok = false;
    std::string error; // ok == false 일 때 원인 메시지

    // 미리보기(저해상도)일 때 원본 픽셀 크기. 0 이면 width/height 와 동일.
    int fullWidth = 0;
    int fullHeight = 0;

    // 프레임이 2개 이상이면 애니메이션.
    bool animated() const { return frames.size() > 1; }
};

// path 의 이미지를 열어 RGBA 로 디코딩한다.
// 파일 시그니처(매직 바이트)를 보고 포맷을 자동으로 판별한다.
// mt=false 이면 단일 스레드로, mt=true 이면 코덱이 지원하는 범위에서
// 가용 CPU를 사용하는 멀티스레드로 디코딩한다.
DecodedImage decode_image(const std::string &path, bool mt=false);

// 빠른 저해상도 미리보기를 디코딩한다(JPEG 만 지원, 그 외/작은 이미지는 ok=false).
// 즉시 화면에 띄우고, 뒤에서 decode_image() 로 전체 해상도를 받아 교체하는 용도.
DecodedImage decode_preview(const std::string &path);
