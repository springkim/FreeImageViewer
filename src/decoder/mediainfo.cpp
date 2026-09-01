#include "decoder/mediainfo.h"

// Bundled MediaInfoLib is built with ENABLE_UNICODE=ON.  Keep these defines
// local to this translation unit so its C++ ABI matches the static library
// without changing the Win32 character mode of the rest of the application.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <MediaInfo/MediaInfo.h>
#include <ZenLib/Ztring.h>

#include <cstddef>

namespace {
    std::string to_utf8(const MediaInfoLib::String& value) {
        return ZenLib::Ztring(value).To_UTF8();
    }

    void collect_stream(MediaInfoLib::MediaInfo& mediaInfo,
                        MediaInfoLib::stream_t kind,
                        const std::string& prefix,
                        std::map<std::string, std::string>& result) {
        if (mediaInfo.Count_Get(kind) == 0) {
            return;
        }

        constexpr size_t streamIndex = 0;
        const size_t fieldCount = mediaInfo.Count_Get(kind, streamIndex);
        for (size_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
            const std::string name = to_utf8(mediaInfo.Get(
                kind, streamIndex, fieldIndex, MediaInfoLib::Info_Name));
            const std::string value = to_utf8(mediaInfo.Get(
                kind, streamIndex, fieldIndex, MediaInfoLib::Info_Text));
            if (!name.empty() && !value.empty()) {
                result.insert_or_assign(prefix + '.' + name, value);
            }
        }
    }
} // namespace

std::map<std::string, std::string> get_mediainfo(const std::string& path) {
    std::map<std::string, std::string> result;
    MediaInfoLib::MediaInfo mediaInfo;

    const ZenLib::Ztring filePath(path.c_str());
    if (mediaInfo.Open(filePath) == 0) {
        return result;
    }

    collect_stream(mediaInfo, MediaInfoLib::Stream_General, "General", result);
    collect_stream(mediaInfo, MediaInfoLib::Stream_Image, "Image", result);

    mediaInfo.Close();
    return result;
}
