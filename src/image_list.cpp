#include<core/image_list.h>

namespace core {
    std::vector<std::string> get_image_file_list(const std::string &image_file) {
        namespace fs = std::filesystem;


        std::error_code ec;

        const fs::path input(image_file);

        fs::path dir = input.parent_path();

        if (dir.empty())
            dir = ".";

        std::vector<ImageFile> files;

        /*
         * directory_iterator 생성 시 예외를 발생시키지 않도록 error_code 사용.
         */
        fs::directory_iterator it(
            dir,
            fs::directory_options::skip_permission_denied,
            ec
        );

        if (ec)
            return {};

        const fs::directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;

            /*
             * extension부터 검사한다.
             *
             * 이미지가 아닌 파일에 대해서는 is_regular_file() 호출을
             * 하지 않으므로 필요 없는 filesystem metadata 조회를 줄인다.
             */
            const std::string ext = entry.path().extension().string();

            if (!is_image_extension(ext))
                continue;

            std::error_code type_ec;

            if (!entry.is_regular_file(type_ec))
                continue;

            /*
             * 정렬용 filename과 반환용 전체 path를 한 번만 생성한다.
             */
            files.push_back({
                entry.path().filename().string(),
                entry.path().string()
            });
        }

        std::sort(
            files.begin(),
            files.end(),
            [](const ImageFile &a, const ImageFile &b) noexcept {
                return natural_less(a.name, b.name);
            }
        );
        std::vector<std::string> result;
        result.reserve(files.size());

        for (auto &file: files)
            result.emplace_back(std::move(file.path));


        return result;
    }
}
