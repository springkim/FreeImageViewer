## 크로스플랫폼 이미지 뷰어

### Linux, Windows, Mac에서 모두 빌드가 가능하고 실행할 수 있어야 한다.

 * 빌드
   * Windows에서는 MinGW gcc/g++만 사용한다.
   * Linux에서는 gcc/g++을 사용한다.
   * MacOS에서는 Apple(clang, clang++)을 사용한다.
   * 따라서 CMakeLists.txt에서 MSVC나 다른 컴파일러를 고려하지 않아도 된다.

* GUI 개발
  * MacOS에서는 OBJC/OBJC++을 이용해서 Cocoa로 앱을 만든다.
  * Windows는 Win32API를 사용해서 앱을 만든다.
  * Linux는 GTK4로 GUI앱을 만든다.
