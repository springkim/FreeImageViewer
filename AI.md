## 크로스플랫폼 이미지 뷰어

### Linux, Windows, Mac에서 모두 빌드가 가능하고 실행할 수 있어야 한다.

 * 빌드
   * Windows에서는 MinGW gcc/g++만 사용한다.
   * Linux에서는 gcc/g++을 사용한다.
   * MacOS에서는 Apple(clang, clang++)을 사용한다.
   * 따라서 CMakeLists.txt에서 MSVC나 다른 컴파일러를 고려하지 않아도 된다.
* GUI 개발
  * 시작 함수는 C++의 main 함수이다.
  * 이미지 처리
    * `DecodedImage` 구조체를 기본으로 사용한다.
      * cocoa를 사용할 때는 `NSImageView`로 변환해서 화면에 띄운다.
      * win32를 사용할 때는 `StretchDIBits`를 사용한다.
    * GIF의 애니메이션이 재생될 수 있어야 한다.
  * MacOS에서는 OBJC/OBJC++을 이용해서 Cocoa로 앱을 만든다.
    * `gui/cocoa/`디렉터리에 objc, objc++ 파일들을 작성한다.
  * Windows는 Win32API를 사용해서 앱을 만든다.
    * `gui/win32`디렉터리에 WIN32API를 이용한 C++ 파일들을 작성한다.
  * Linux는 GTK4로 GUI앱을 만든다.
    * `gui/gtk4`디렉터리에 GTK4를 이용한 C++파일들을 작성한다.
  * 이미지 뷰어 창
    * 이미지는 캡션바를 제외한 영역에서 윈도우 창에 fit하게 채워져야 한다.
    * 창의 크기가 변경되면 이미지 뷰어의 크기도 같이 변경되어야 한다.
* GUI 개발 단계
  * 1단계
    * void imshow(const char* image_path) 함수를 헤더에 정의하고 objc++에서 지정된 경로의 이미지 크기랑 똑같은 크기의 창을 띄우고 image를 화면에 그린다.
      * 캡션바는 항상 존재하고 종료버튼을 눌러야 함수가 종료된다.
  
