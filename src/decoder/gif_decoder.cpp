//
// gif_decoder.cpp
// giflib 로 GIF 를 디코딩한다. 멀티스레드 모드에서는 서로 독립적인 각 프레임의
// LZW 스트림을 병렬로 해제하고, disposal/투명 색인 합성은 원래 순서대로 수행한다.
//
#include "decoder/gif_decoder.h"
#include "thread_count.h"

#include <gif_lib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
    struct GifFrameView {
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        const GifByteType* raster = nullptr;
        const GifColorType* colors = nullptr;
        int color_count = 0;
        int transparent = NO_TRANSPARENT_COLOR;
        int disposal = DISPOSAL_UNSPECIFIED;
        int delay_cs = 0;
    };

    struct DecodedGifFrame {
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        std::vector<GifByteType> raster;
        std::array<GifColorType, 256> colors{};
        int color_count = 0;
        int transparent = NO_TRANSPARENT_COLOR;
        int disposal = DISPOSAL_UNSPECIFIED;
        int delay_cs = 0;
        bool ok = false;
    };

    struct GifFrameSlice {
        size_t gce_begin = 0;
        size_t gce_end = 0;
        size_t image_begin = 0;
        size_t image_end = 0;
    };

    struct GifFrameIndex {
        size_t prefix_end = 0;
        int canvas_width = 0;
        int canvas_height = 0;
        std::vector<GifFrameSlice> frames;
    };

    struct MemorySpan {
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    struct GifMemoryReader {
        std::array<MemorySpan, 4> spans{};
        size_t span_count = 0;
        size_t span_index = 0;
        size_t offset = 0;
    };

    bool read_file(const std::string& path, std::vector<uint8_t>& data) {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        if (std::fseek(file, 0, SEEK_END) != 0) {
            std::fclose(file);
            return false;
        }
        const long file_size = std::ftell(file);
        if (file_size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
            std::fclose(file);
            return false;
        }
        data.resize(static_cast<size_t>(file_size));
        const size_t read_size = std::fread(data.data(), 1, data.size(), file);
        std::fclose(file);
        return read_size == data.size();
    }

    uint16_t read_le16(const uint8_t* data) {
        return static_cast<uint16_t>(data[0]) |
               (static_cast<uint16_t>(data[1]) << 8U);
    }

    bool skip_sub_blocks(const std::vector<uint8_t>& data, size_t& offset) {
        while (offset < data.size()) {
            const size_t block_size = data[offset++];
            if (block_size == 0) {
                return true;
            }
            if (block_size > data.size() - offset) {
                return false;
            }
            offset += block_size;
        }
        return false;
    }

    // GIF 컨테이너만 가볍게 훑어 각 image descriptor와 LZW sub-block 범위를 찾는다.
    // 실제 LZW 해제와 유효성 검사는 각 worker의 giflib가 담당한다.
    bool index_gif_frames(const std::vector<uint8_t>& data, GifFrameIndex& index) {
        if (data.size() < 13 ||
            (std::memcmp(data.data(), "GIF87a", 6) != 0 &&
             std::memcmp(data.data(), "GIF89a", 6) != 0)) {
            return false;
        }

        index.canvas_width = read_le16(data.data() + 6);
        index.canvas_height = read_le16(data.data() + 8);
        if (index.canvas_width <= 0 || index.canvas_height <= 0) {
            return false;
        }

        size_t offset = 13;
        const uint8_t logical_packed = data[10];
        if ((logical_packed & 0x80U) != 0) {
            const size_t color_count = size_t{1} << ((logical_packed & 0x07U) + 1U);
            const size_t color_bytes = color_count * 3;
            if (color_bytes > data.size() - offset) {
                return false;
            }
            offset += color_bytes;
        }
        index.prefix_end = offset;

        size_t pending_gce_begin = 0;
        size_t pending_gce_end = 0;
        bool found_trailer = false;

        while (offset < data.size()) {
            const uint8_t marker = data[offset++];
            if (marker == 0x3bU) {
                found_trailer = true;
                break;
            }

            if (marker == 0x21U) {
                const size_t extension_begin = offset - 1;
                if (offset >= data.size()) {
                    return false;
                }
                const uint8_t label = data[offset++];
                if (!skip_sub_blocks(data, offset)) {
                    return false;
                }
                if (label == 0xf9U) {
                    pending_gce_begin = extension_begin;
                    pending_gce_end = offset;
                }
                continue;
            }

            if (marker != 0x2cU || data.size() - offset < 9) {
                return false;
            }

            GifFrameSlice frame;
            frame.gce_begin = pending_gce_begin;
            frame.gce_end = pending_gce_end;
            frame.image_begin = offset - 1;

            const uint8_t image_packed = data[offset + 8];
            offset += 9;
            if ((image_packed & 0x80U) != 0) {
                const size_t color_count = size_t{1} << ((image_packed & 0x07U) + 1U);
                const size_t color_bytes = color_count * 3;
                if (color_bytes > data.size() - offset) {
                    return false;
                }
                offset += color_bytes;
            }

            if (offset >= data.size()) {
                return false;
            }
            ++offset; // LZW minimum code size
            if (!skip_sub_blocks(data, offset)) {
                return false;
            }
            frame.image_end = offset;
            index.frames.push_back(frame);
            pending_gce_begin = pending_gce_end = 0;
        }

        return found_trailer && !index.frames.empty();
    }

    int read_gif_memory(GifFileType* gif, GifByteType* output, int requested) {
        if (!gif || !output || requested <= 0) {
            return 0;
        }
        auto* reader = static_cast<GifMemoryReader*>(gif->UserData);
        int copied = 0;
        while (copied < requested && reader->span_index < reader->span_count) {
            const MemorySpan& span = reader->spans[reader->span_index];
            if (reader->offset >= span.size) {
                ++reader->span_index;
                reader->offset = 0;
                continue;
            }
            const size_t count = std::min<size_t>(
                static_cast<size_t>(requested - copied), span.size - reader->offset);
            std::memcpy(output + copied, span.data + reader->offset, count);
            copied += static_cast<int>(count);
            reader->offset += count;
        }
        return copied;
    }

    bool decode_indexed_frame(const std::vector<uint8_t>& data,
                              const GifFrameIndex& index, size_t frame_index,
                              DecodedGifFrame& output) {
        static const uint8_t trailer = 0x3b;
        const GifFrameSlice& slice = index.frames[frame_index];

        GifMemoryReader reader;
        reader.spans[reader.span_count++] = {data.data(), index.prefix_end};
        if (slice.gce_end > slice.gce_begin) {
            reader.spans[reader.span_count++] = {
                data.data() + slice.gce_begin, slice.gce_end - slice.gce_begin};
        }
        reader.spans[reader.span_count++] = {
            data.data() + slice.image_begin, slice.image_end - slice.image_begin};
        reader.spans[reader.span_count++] = {&trailer, 1};

        int error = 0;
        GifFileType* gif = DGifOpen(&reader, read_gif_memory, &error);
        if (!gif) {
            return false;
        }
        if (DGifSlurp(gif) != GIF_OK || gif->ImageCount != 1) {
            DGifCloseFile(gif, &error);
            return false;
        }

        const SavedImage& frame = gif->SavedImages[0];
        const ColorMapObject* color_map =
            frame.ImageDesc.ColorMap ? frame.ImageDesc.ColorMap : gif->SColorMap;
        const int width = frame.ImageDesc.Width;
        const int height = frame.ImageDesc.Height;
        if (!color_map || !frame.RasterBits || width <= 0 || height <= 0 ||
            color_map->ColorCount <= 0 || color_map->ColorCount > 256 ||
            static_cast<size_t>(width) >
                std::numeric_limits<size_t>::max() / static_cast<size_t>(height)) {
            DGifCloseFile(gif, &error);
            return false;
        }

        output.left = frame.ImageDesc.Left;
        output.top = frame.ImageDesc.Top;
        output.width = width;
        output.height = height;
        output.color_count = color_map->ColorCount;
        std::copy_n(color_map->Colors, output.color_count, output.colors.begin());
        output.raster.assign(frame.RasterBits,
                             frame.RasterBits + static_cast<size_t>(width) * height);

        GraphicsControlBlock control{};
        if (DGifSavedExtensionToGCB(gif, 0, &control) == GIF_OK) {
            output.transparent = control.TransparentColor;
            output.disposal = control.DisposalMode;
            output.delay_cs = control.DelayTime;
        }
        output.ok = true;
        DGifCloseFile(gif, &error);
        return true;
    }

    bool decode_frames_parallel(const std::vector<uint8_t>& data,
                                const GifFrameIndex& index,
                                std::vector<DecodedGifFrame>& frames) {
        frames.resize(index.frames.size());
        const size_t thread_count = std::min<size_t>(
            {static_cast<size_t>(decoder_detail::available_thread_count()),
             frames.size(), 32});
        if (thread_count < 2) {
            return false;
        }

        std::atomic<size_t> next_frame{0};
        std::atomic<bool> failed{false};
        auto worker = [&] {
            while (!failed.load(std::memory_order_relaxed)) {
                const size_t i = next_frame.fetch_add(1, std::memory_order_relaxed);
                if (i >= frames.size()) {
                    return;
                }
                if (!decode_indexed_frame(data, index, i, frames[i])) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(thread_count - 1);
        try {
            for (size_t i = 1; i < thread_count; ++i) {
                workers.emplace_back(worker);
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            for (std::thread& thread : workers) {
                thread.join();
            }
            return false;
        }
        worker();
        for (std::thread& thread : workers) {
            thread.join();
        }
        return !failed.load(std::memory_order_relaxed) &&
               std::all_of(frames.begin(), frames.end(),
                           [](const DecodedGifFrame& frame) { return frame.ok; });
    }

    GifFrameView make_view(const DecodedGifFrame& frame) {
        return {frame.left, frame.top, frame.width, frame.height,
                frame.raster.data(), frame.colors.data(), frame.color_count,
                frame.transparent, frame.disposal, frame.delay_cs};
    }

    GifFrameView make_view(GifFileType* gif, int frame_index) {
        const SavedImage& frame = gif->SavedImages[frame_index];
        const ColorMapObject* color_map =
            frame.ImageDesc.ColorMap ? frame.ImageDesc.ColorMap : gif->SColorMap;
        GraphicsControlBlock control{};
        const bool has_control =
            DGifSavedExtensionToGCB(gif, frame_index, &control) == GIF_OK;
        return {frame.ImageDesc.Left, frame.ImageDesc.Top,
                frame.ImageDesc.Width, frame.ImageDesc.Height,
                frame.RasterBits, color_map ? color_map->Colors : nullptr,
                color_map ? color_map->ColorCount : 0,
                has_control ? control.TransparentColor : NO_TRANSPARENT_COLOR,
                has_control ? control.DisposalMode : DISPOSAL_UNSPECIFIED,
                has_control ? control.DelayTime : 0};
    }

    struct ClippedRect {
        int src_x = 0;
        int src_y = 0;
        int dst_x = 0;
        int dst_y = 0;
        int width = 0;
        int height = 0;
    };

    ClippedRect clip_frame(const GifFrameView& frame, int canvas_width,
                           int canvas_height) {
        ClippedRect rect;
        rect.src_x = std::max(0, -frame.left);
        rect.src_y = std::max(0, -frame.top);
        rect.dst_x = std::max(0, frame.left);
        rect.dst_y = std::max(0, frame.top);
        rect.width = std::max(0, std::min(frame.width - rect.src_x,
                                          canvas_width - rect.dst_x));
        rect.height = std::max(0, std::min(frame.height - rect.src_y,
                                           canvas_height - rect.dst_y));
        return rect;
    }

    void save_rect(const std::vector<uint8_t>& canvas, int canvas_width,
                   const ClippedRect& rect, std::vector<uint8_t>& saved) {
        const size_t row_bytes = static_cast<size_t>(rect.width) * 4;
        saved.resize(row_bytes * rect.height);
        for (int y = 0; y < rect.height; ++y) {
            const uint8_t* source = canvas.data() +
                (static_cast<size_t>(rect.dst_y + y) * canvas_width + rect.dst_x) * 4;
            std::memcpy(saved.data() + static_cast<size_t>(y) * row_bytes,
                        source, row_bytes);
        }
    }

    void restore_rect(std::vector<uint8_t>& canvas, int canvas_width,
                      const ClippedRect& rect, const std::vector<uint8_t>& saved) {
        const size_t row_bytes = static_cast<size_t>(rect.width) * 4;
        if (saved.size() != row_bytes * rect.height) {
            return;
        }
        for (int y = 0; y < rect.height; ++y) {
            uint8_t* destination = canvas.data() +
                (static_cast<size_t>(rect.dst_y + y) * canvas_width + rect.dst_x) * 4;
            std::memcpy(destination,
                        saved.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
        }
    }

    void clear_rect(std::vector<uint8_t>& canvas, int canvas_width,
                    const ClippedRect& rect) {
        const size_t row_bytes = static_cast<size_t>(rect.width) * 4;
        for (int y = 0; y < rect.height; ++y) {
            uint8_t* destination = canvas.data() +
                (static_cast<size_t>(rect.dst_y + y) * canvas_width + rect.dst_x) * 4;
            std::memset(destination, 0, row_bytes);
        }
    }

    void draw_frame(std::vector<uint8_t>& canvas, int canvas_width,
                    const GifFrameView& frame, const ClippedRect& rect) {
        if (!frame.raster || !frame.colors || frame.color_count <= 0) {
            return;
        }
        for (int y = 0; y < rect.height; ++y) {
            const GifByteType* source = frame.raster +
                static_cast<size_t>(rect.src_y + y) * frame.width + rect.src_x;
            uint8_t* destination = canvas.data() +
                (static_cast<size_t>(rect.dst_y + y) * canvas_width + rect.dst_x) * 4;
            for (int x = 0; x < rect.width; ++x) {
                const int color_index = source[x];
                if (color_index == frame.transparent || color_index >= frame.color_count) {
                    continue;
                }
                const GifColorType& color = frame.colors[color_index];
                destination[x * 4] = color.Red;
                destination[x * 4 + 1] = color.Green;
                destination[x * 4 + 2] = color.Blue;
                destination[x * 4 + 3] = 255;
            }
        }
    }

    template <typename ViewAt>
    bool compose_frames(DecodedImage& image, int canvas_width, int canvas_height,
                        size_t frame_count, ViewAt view_at) {
        if (canvas_width <= 0 || canvas_height <= 0 || frame_count == 0 ||
            static_cast<size_t>(canvas_width) >
                std::numeric_limits<size_t>::max() / static_cast<size_t>(canvas_height) / 4) {
            return false;
        }

        image.width = canvas_width;
        image.height = canvas_height;
        image.frames.reserve(frame_count);
        std::vector<uint8_t> canvas(
            static_cast<size_t>(canvas_width) * canvas_height * 4, 0);
        std::vector<uint8_t> saved;

        for (size_t i = 0; i < frame_count; ++i) {
            const GifFrameView frame = view_at(i);
            const ClippedRect rect = clip_frame(frame, canvas_width, canvas_height);
            if (frame.disposal == DISPOSE_PREVIOUS) {
                save_rect(canvas, canvas_width, rect, saved);
            }

            draw_frame(canvas, canvas_width, frame, rect);

            ImageFrame output;
            output.pixels = canvas;
            output.delay_ms = frame.delay_cs * 10;
            if (output.delay_ms <= 10) {
                output.delay_ms = 100;
            }
            image.frames.push_back(std::move(output));

            if (frame.disposal == DISPOSE_BACKGROUND) {
                clear_rect(canvas, canvas_width, rect);
            } else if (frame.disposal == DISPOSE_PREVIOUS) {
                restore_rect(canvas, canvas_width, rect, saved);
            }
        }
        return !image.frames.empty();
    }

    bool decode_gif_parallel(const std::string& path, DecodedImage& image) {
        std::vector<uint8_t> data;
        GifFrameIndex index;
        if (!read_file(path, data) || !index_gif_frames(data, index)) {
            return false;
        }

        std::vector<DecodedGifFrame> frames;
        if (index.frames.size() == 1) {
            frames.resize(1);
            if (!decode_indexed_frame(data, index, 0, frames.front())) {
                return false;
            }
        } else {
            if (!decode_frames_parallel(data, index, frames)) {
                return false;
            }
        }

        return compose_frames(image, index.canvas_width, index.canvas_height,
                              frames.size(), [&](size_t i) {
            return make_view(frames[i]);
        });
    }

    DecodedImage decode_gif_sequential(const std::string& path) {
        DecodedImage image;
        int error = 0;
        GifFileType* gif = DGifOpenFileName(path.c_str(), &error);
        if (!gif) {
            image.error = std::string("GIF 열기 실패: ") + GifErrorString(error);
            return image;
        }
        if (DGifSlurp(gif) != GIF_OK) {
            image.error = std::string("GIF 파싱 실패: ") + GifErrorString(gif->Error);
            DGifCloseFile(gif, &error);
            return image;
        }
        if (gif->ImageCount < 1) {
            image.error = "GIF 에 이미지가 없습니다";
            DGifCloseFile(gif, &error);
            return image;
        }

        const int width = gif->SWidth;
        const int height = gif->SHeight;
        if (width <= 0 || height <= 0) {
            image.error = "유효하지 않은 GIF 크기입니다";
            DGifCloseFile(gif, &error);
            return image;
        }

        const bool composed = compose_frames(
            image, width, height, static_cast<size_t>(gif->ImageCount),
            [&](size_t i) { return make_view(gif, static_cast<int>(i)); });
        DGifCloseFile(gif, &error);
        if (!composed) {
            image.error = "GIF 디코딩 실패";
            image.frames.clear();
            return image;
        }

        image.pixels = image.frames.front().pixels;
        image.ok = true;
        return image;
    }
} // namespace

DecodedImage decode_gif(const std::string& path, bool mt) {
    if (mt) {
        std::error_code size_error;
        const uintmax_t file_size = std::filesystem::file_size(path, size_error);
        if (!size_error && file_size >= 64U * 1024U) {
            DecodedImage image;
            if (decode_gif_parallel(path, image)) {
                image.pixels = image.frames.front().pixels;
                image.ok = true;
                return image;
            }
        }
    }
    return decode_gif_sequential(path);
}
