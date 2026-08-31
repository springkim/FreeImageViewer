#pragma once
//
// jpeg_decoder.h
// libjpeg-turbo(turbojpeg 3.x API)를 이용해 JPEG 파일을 RGBA 픽셀로 디코딩한다.
//
#include "image_decoder.h"

// path 의 JPEG 파일을 열어 RGBA 로 디코딩한다.
DecodedImage decode_jpeg(const std::string& path);

// 큰 JPEG 을 1/8 해상도로 빠르게 디코딩한 미리보기를 반환한다.
// 원본이 작으면(미리보기 이득이 없으면) ok=false. fullWidth/fullHeight 에 원본 크기를 담는다.
DecodedImage decode_jpeg_preview(const std::string& path);
