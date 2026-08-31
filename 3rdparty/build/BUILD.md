  적용 사항:

  - Linux: GCC/G++ + Unix Makefiles
  - macOS: Clang/Clang++ + Unix Makefiles
  - Windows: GCC/G++ + MinGW Makefiles
  - 다운로드 캐시와 병렬 빌드 지원
  - 헤더를 ../include로 복사
  - 모든 .a, .lib와 종속 라이브러리를 ../lib로 복사
  - 14개 모두 Perl 문법 및 실행 분기 검사 통과
  - macOS에서 zlib, libspng, libjpeg-turbo, libwebp, zstd, libtiff, libgif, openjpeg 실제 빌드 완료
  - 기존 .bash/.zsh/.bat 파일은 보존



### libtiff 빌드 순서:

perl build_zlib.pl
perl build_libjpeg-turbo.pl
perl build_libwebp.pl
perl build_zstd.pl
perl build_libtiff.pl

### 빌드 전 설정 확인:

perl build_libavif.pl --print-config

### OpenEXR 빌드:

perl build_openexr.pl

### stb_image 헤더 설치:

perl build_stb.pl


### MacOS
```
brew install nasm
```

### Linux
```
sudo apt install meson nasm -y
```