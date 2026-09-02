#pragma once

// image_path의 이미지를 GTK4 창에 표시한다.
// 창이 닫힐 때까지 호출한 스레드를 차단한다.
void imshow(const char* image_path);
