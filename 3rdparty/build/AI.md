# 빌드 규칙



### perl파일 1개로 Linux,MacOS,Windows 모두에서 빌드가 가능하게 한다.

`.bat`으로 끝나는 Windows CMD, `.bash`로 끝나는 리눅스 쉘, `.zsh`로 끝나는 맥북에서의 빌드 스크립트를 모두 하나의 perl 스크립트로 만든다.

* CMake빌드

  * Linux에서는 Unix Makefiles를 제너레이터로 사용하고 C/C++컴파일러는 gcc, g++을 사용한다.
  * MacOS에서는 Unix Makefiles를 제너레이터로 사용하고 C/C++컴파일러는 clang/clang++을 사용한다.
  * Windows에서는 MinGW Makefiles를 제너레이터로 사용하고 C/C++컴파일러는 gcc/g++을 사용한다.

* Static 라이브러리

  * 3rdparty에 포함되어 있는 include속 폴더는 현재 디렉터리의 부모 디렉터리에 존재하는 `../include`폴더에 모두 복사한다.
  * lib에 `*.a`나 `*.lib`파일들은 의존성을 전부 찾아서 `../lib`폴더에 모두 복사한다.

  



### 추가 지원 포맷

* EXR
  * https://github.com/academysoftwarefoundation/openexr
* 
