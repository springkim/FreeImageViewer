// ICO 자체는 stb_image가 직접 지원하지 않으므로 컨테이너만 해석한 뒤,
// 내부 PNG 또는 DIB를 BMP 스트림으로 만들어 stb_image에 넘긴다.
#include "decoder/ico_decoder.h"
#include "decoder/stb_decoder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace {
    constexpr uint8_t kPngSignature[8] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
    };

    uint16_t read_le16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) |
               static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
    }

    uint32_t read_le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    int32_t read_le_i32(const uint8_t* p) {
        const uint32_t value = read_le32(p);
        int32_t result = 0;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }

    void write_le16(uint8_t* p, uint16_t value) {
        p[0] = static_cast<uint8_t>(value);
        p[1] = static_cast<uint8_t>(value >> 8);
    }

    void write_le32(uint8_t* p, uint32_t value) {
        p[0] = static_cast<uint8_t>(value);
        p[1] = static_cast<uint8_t>(value >> 8);
        p[2] = static_cast<uint8_t>(value >> 16);
        p[3] = static_cast<uint8_t>(value >> 24);
    }

    bool read_file(const std::string& path, std::vector<uint8_t>& data) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return false;
        }
        const std::streamoff end = file.tellg();
        if (end <= 0 || static_cast<uint64_t>(end) >
                            static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        data.resize(static_cast<size_t>(end));
        file.seekg(0, std::ios::beg);
        return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), end));
    }

    struct IconEntry {
        uint32_t offset = 0;
        uint32_t size = 0;
        int width = 0;
        int height = 0;
        int bit_count = 0;
    };

    struct DibLayout {
        int width = 0;
        int height = 0;
        int bit_count = 0;
        bool bottom_up = true;
        size_t and_mask_offset = 0;
        size_t and_mask_stride = 0;
        bool has_and_mask = false;
    };

    bool make_bmp_from_icon_dib(const uint8_t* dib, size_t dib_size,
                                std::vector<uint8_t>& bmp, DibLayout& layout) {
        if (dib_size < 12) {
            return false;
        }

        const uint32_t header_size = read_le32(dib);
        if (header_size > dib_size || (header_size != 12 && header_size < 40)) {
            return false;
        }

        uint64_t palette_entries = 0;
        uint64_t palette_entry_size = 0;
        uint64_t mask_bytes = 0;
        uint32_t compression = 0;

        if (header_size == 12) {
            layout.width = read_le16(dib + 4);
            const uint16_t doubled_height = read_le16(dib + 6);
            if (doubled_height == 0 || (doubled_height & 1U) != 0) {
                return false;
            }
            layout.height = doubled_height / 2;
            layout.bit_count = read_le16(dib + 10);
            palette_entry_size = 3;
            if (layout.bit_count > 0 && layout.bit_count <= 8) {
                palette_entries = uint64_t{1} << layout.bit_count;
            }
        } else {
            const int32_t dib_width = read_le_i32(dib + 4);
            const int32_t doubled_height = read_le_i32(dib + 8);
            if (dib_width <= 0 || doubled_height == 0) {
                return false;
            }
            const int64_t height64 = doubled_height;
            const uint64_t absolute_height = height64 < 0
                ? static_cast<uint64_t>(-height64)
                : static_cast<uint64_t>(height64);
            if ((absolute_height & 1U) != 0 || absolute_height / 2 >
                    static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                return false;
            }
            layout.width = dib_width;
            layout.height = static_cast<int>(absolute_height / 2);
            layout.bottom_up = doubled_height > 0;
            layout.bit_count = read_le16(dib + 14);
            compression = read_le32(dib + 16);
            const uint32_t colors_used = read_le32(dib + 32);
            palette_entry_size = 4;
            if (colors_used != 0) {
                palette_entries = colors_used;
            } else if (layout.bit_count > 0 && layout.bit_count <= 8) {
                palette_entries = uint64_t{1} << layout.bit_count;
            }
            // BITMAPINFOHEADER 뒤에 저장되는 비트필드 마스크.
            if (header_size == 40 && compression == 3) {
                mask_bytes = 12;
            } else if (header_size == 40 && compression == 6) {
                mask_bytes = 16;
            }
        }

        if (layout.width <= 0 || layout.height <= 0 || layout.bit_count <= 0) {
            return false;
        }

        const uint64_t pixel_offset64 = static_cast<uint64_t>(header_size) + mask_bytes +
                                        palette_entries * palette_entry_size;
        if (pixel_offset64 > dib_size || dib_size >
                static_cast<size_t>(std::numeric_limits<uint32_t>::max() - 14U)) {
            return false;
        }
        const size_t pixel_offset = static_cast<size_t>(pixel_offset64);

        bmp.assign(14 + dib_size, 0);
        bmp[0] = 'B';
        bmp[1] = 'M';
        write_le32(bmp.data() + 2, static_cast<uint32_t>(bmp.size()));
        write_le32(bmp.data() + 10, static_cast<uint32_t>(14 + pixel_offset));
        std::memcpy(bmp.data() + 14, dib, dib_size);

        // ICO DIB의 높이는 XOR 이미지와 AND 마스크를 합쳐 두 배로 기록된다.
        if (header_size == 12) {
            write_le16(bmp.data() + 14 + 6, static_cast<uint16_t>(layout.height));
        } else {
            const int32_t bmp_height = layout.bottom_up ? layout.height : -layout.height;
            write_le32(bmp.data() + 14 + 8, static_cast<uint32_t>(bmp_height));
        }

        // 비압축 DIB이면 XOR 픽셀 바로 뒤의 1비트 AND 투명 마스크도 적용할 수 있다.
        if (header_size == 12 || compression == 0 || compression == 3 || compression == 6) {
            const uint64_t xor_stride =
                (static_cast<uint64_t>(layout.width) * layout.bit_count + 31) / 32 * 4;
            const uint64_t xor_size = xor_stride * static_cast<uint64_t>(layout.height);
            const uint64_t and_stride =
                (static_cast<uint64_t>(layout.width) + 31) / 32 * 4;
            const uint64_t and_size = and_stride * static_cast<uint64_t>(layout.height);
            const uint64_t and_offset = pixel_offset64 + xor_size;
            if (and_offset <= dib_size && and_size <= dib_size - and_offset) {
                layout.and_mask_offset = static_cast<size_t>(and_offset);
                layout.and_mask_stride = static_cast<size_t>(and_stride);
                layout.has_and_mask = true;
            }
        }
        return true;
    }

    void apply_and_mask(const uint8_t* dib, const DibLayout& layout,
                        DecodedImage& image) {
        if (!layout.has_and_mask || image.width != layout.width ||
            image.height != layout.height) {
            return;
        }
        const uint8_t* mask = dib + layout.and_mask_offset;
        for (int y = 0; y < layout.height; ++y) {
            const int source_y = layout.bottom_up ? layout.height - 1 - y : y;
            const uint8_t* row = mask + static_cast<size_t>(source_y) * layout.and_mask_stride;
            for (int x = 0; x < layout.width; ++x) {
                if ((row[x / 8] & (0x80U >> (x & 7))) != 0) {
                    image.pixels[(static_cast<size_t>(y) * layout.width + x) * 4 + 3] = 0;
                }
            }
        }
    }
}

DecodedImage decode_ico(const std::string& path) {
    DecodedImage result;
    std::vector<uint8_t> file;
    if (!read_file(path, file)) {
        result.error = "ICO 파일을 읽을 수 없거나 파일이 너무 큽니다: " + path;
        return result;
    }
    if (file.size() < 6 || read_le16(file.data()) != 0 ||
        read_le16(file.data() + 2) != 1) {
        result.error = "올바른 ICO 파일이 아닙니다: " + path;
        return result;
    }

    const uint16_t count = read_le16(file.data() + 4);
    if (count == 0 || static_cast<uint64_t>(count) * 16 + 6 > file.size()) {
        result.error = "ICO 디렉터리가 손상되었습니다: " + path;
        return result;
    }

    std::vector<IconEntry> entries;
    entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = file.data() + 6 + static_cast<size_t>(i) * 16;
        IconEntry candidate;
        candidate.width = entry[0] == 0 ? 256 : entry[0];
        candidate.height = entry[1] == 0 ? 256 : entry[1];
        candidate.bit_count = read_le16(entry + 6);
        candidate.size = read_le32(entry + 8);
        candidate.offset = read_le32(entry + 12);
        if (candidate.size != 0 && candidate.offset <= file.size() &&
            candidate.size <= file.size() - candidate.offset) {
            entries.push_back(candidate);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const IconEntry& a,
                                                         const IconEntry& b) {
        const int64_t area_a = static_cast<int64_t>(a.width) * a.height;
        const int64_t area_b = static_cast<int64_t>(b.width) * b.height;
        return area_a != area_b ? area_a > area_b : a.bit_count > b.bit_count;
    });

    std::string last_error;
    for (const IconEntry& entry : entries) {
        const uint8_t* data = file.data() + entry.offset;
        const size_t size = entry.size;
        if (size >= sizeof(kPngSignature) &&
            std::memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0) {
            result = decoder_detail::decode_stb_memory(data, size, "ICO/PNG");
        } else {
            std::vector<uint8_t> bmp;
            DibLayout layout;
            if (!make_bmp_from_icon_dib(data, size, bmp, layout)) {
                last_error = "지원하지 않거나 손상된 ICO DIB";
                continue;
            }
            result = decoder_detail::decode_stb_memory(bmp.data(), bmp.size(), "ICO/BMP");
            if (result.ok) {
                apply_and_mask(data, layout, result);
            }
        }
        if (result.ok) {
            return result;
        }
        last_error = result.error;
    }

    result = {};
    result.error = "ICO 이미지 디코딩 실패";
    if (!last_error.empty()) {
        result.error += "(" + last_error + ")";
    }
    result.error += ": " + path;
    return result;
}
