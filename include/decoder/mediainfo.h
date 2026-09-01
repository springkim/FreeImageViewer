#pragma once

#include <map>
#include <string>

// MediaInfo가 감지한 General 및 첫 번째 Image 스트림의 비어 있지 않은 필드를
// UTF-8 문자열 맵으로 반환한다. 키 형식: General.Format, Image.Width 등.
// 파일을 열거나 분석하지 못하면 빈 맵을 반환한다.
std::map<std::string, std::string> get_mediainfo(const std::string& path);
