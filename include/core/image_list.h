#ifndef HG_A01A14508B268A4C9AE78CB8A63F0ACA_H
#define HG_A01A14508B268A4C9AE78CB8A63F0ACA_H

#include <algorithm>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>


namespace core {
    namespace fs = std::filesystem;

    inline char ascii_lower(char c) noexcept {
        if (c >= 'A' && c <= 'Z')
            return (char) (c + ('a' - 'A'));

        return c;
    }

    inline bool natural_less(std::string_view a, std::string_view b) noexcept {
        std::size_t i = 0;
        std::size_t j = 0;

        while (i < a.size() && j < b.size()) {
            const bool a_digit = a[i] >= '0' && a[i] <= '9';
            const bool b_digit = b[j] >= '0' && b[j] <= '9';

            if (a_digit && b_digit) {
                // 숫자 시작 위치
                std::size_t ai = i;
                std::size_t bj = j;

                // leading zero 건너뛰기
                while (ai < a.size() && a[ai] == '0')
                    ++ai;

                while (bj < b.size() && b[bj] == '0')
                    ++bj;

                // 숫자 끝 찾기
                std::size_t ae = ai;
                std::size_t be = bj;

                while (ae < a.size() && a[ae] >= '0' && a[ae] <= '9')
                    ++ae;

                while (be < b.size() && b[be] >= '0' && b[be] <= '9')
                    ++be;

                const std::size_t alen = ae - ai;
                const std::size_t blen = be - bj;

                // 자릿수가 다르면 긴 쪽이 큰 숫자
                if (alen != blen)
                    return alen < blen;

                // 같은 자릿수면 문자 그대로 비교
                for (std::size_t k = 0; k < alen; ++k) {
                    if (a[ai + k] != b[bj + k])
                        return a[ai + k] < b[bj + k];
                }

                // 숫자값이 같다면 전체 숫자 구간을 건너뛴다.
                while (i < a.size() && a[i] >= '0' && a[i] <= '9')
                    ++i;

                while (j < b.size() && b[j] >= '0' && b[j] <= '9')
                    ++j;

                continue;
            }

            const char ac = ascii_lower(a[i]);
            const char bc = ascii_lower(b[j]);

            if (ac != bc)
                return ac < bc;

            ++i;
            ++j;
        }

        return a.size() < b.size();
    }


    inline bool iequals(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size())
            return false;

        for (std::size_t i = 0; i < a.size(); ++i) {
            if (ascii_lower(a[i]) != b[i])
                return false;
        }

        return true;
    }


    inline bool is_image_extension(std::string_view ext) noexcept {
        if (ext.empty())
            return false;

        if (ext[0] == '.')
            ext.remove_prefix(1);

        /*
         * 길이별로 먼저 분기해서 불필요한 문자열 비교를 줄인다.
         *
         * 지원:
         * heif heic avif gif webp jpg jpeg png bmp
         * tiff tif jxl j2k j2c jpc jpf jpx exr qoi jp2
         * pnm pbm pgm ppm pam tga psd
         */
        switch (ext.size()) {
            case 3:
                return
                        iequals(ext, "gif") ||
                        iequals(ext, "jpg") ||
                        iequals(ext, "png") ||
                        iequals(ext, "bmp") ||
                        iequals(ext, "tif") ||
                        iequals(ext, "jxl") ||
                        iequals(ext, "j2k") ||
                        iequals(ext, "j2c") ||
                        iequals(ext, "jpc") ||
                        iequals(ext, "jpf") ||
                        iequals(ext, "jpx") ||
                        iequals(ext, "exr") ||
                        iequals(ext, "qoi") ||
                        iequals(ext, "jp2") ||
                        iequals(ext, "pnm") ||
                        iequals(ext, "pbm") ||
                        iequals(ext, "pgm") ||
                        iequals(ext, "ppm") ||
                        iequals(ext, "pam") ||
                        iequals(ext, "tga") ||
                        iequals(ext, "psd");

            case 4:
                return
                        iequals(ext, "heif") ||
                        iequals(ext, "heic") ||
                        iequals(ext, "avif") ||
                        iequals(ext, "webp") ||
                        iequals(ext, "jpeg") ||
                        iequals(ext, "tiff");

            default:
                return false;
        }
    }


    struct ImageFile {
        std::string name;
        std::string path;
    };


    std::vector<std::string> get_image_file_list(const std::string &image_file);
}


#endif // HG_A01A14508B268A4C9AE78CB8A63F0ACA_H
