//
// gif_decoder.cpp
// giflib 로 GIF 를 디코딩한다. 애니메이션은 각 프레임을 논리 화면(캔버스) 위에
// disposal method / 투명 색인 / 인터레이스를 반영해 합성한 뒤 RGBA 스냅샷으로 만든다.
//
#include "decoder/gif_decoder.h"

#include <gif_lib.h>

#include <cstring>

namespace {
    // 한 프레임(SavedImage)을 캔버스에 그린다.
    // 참고: giflib 5+ 의 DGifSlurp 은 인터레이스 GIF 를 디코딩하면서 이미 화면
    // 순서로 재배열해 RasterBits 에 저장하므로, 여기서는 순차 복사만 하면 된다.
    // (여기서 다시 인터레이스 매핑을 하면 이중 처리로 이미지가 깨진다)
    void drawFrame(std::vector<uint8_t>& canvas, int canvasW, int canvasH,
                   const SavedImage& frame, const ColorMapObject* cmap,
                   int transparent) {
        const GifImageDesc& d = frame.ImageDesc;
        if (!cmap || !frame.RasterBits) {
            return;
        }

        auto blitRow = [&](int srcRow, int dstRow) {
            if (dstRow < 0 || dstRow >= canvasH) {
                return;
            }
            const GifByteType* line = frame.RasterBits + (size_t)srcRow * d.Width;
            for (int x = 0; x < d.Width; ++x) {
                int px = d.Left + x;
                if (px < 0 || px >= canvasW) {
                    continue;
                }
                int idx = line[x];
                if (idx == transparent || idx >= cmap->ColorCount) {
                    continue;   // 투명/범위밖 → 캔버스 기존 픽셀 유지
                }
                const GifColorType& c = cmap->Colors[idx];
                uint8_t* out = &canvas[((size_t)dstRow * canvasW + px) * 4];
                out[0] = c.Red;
                out[1] = c.Green;
                out[2] = c.Blue;
                out[3] = 255;
            }
        };

        for (int y = 0; y < d.Height; ++y) {
            blitRow(y, d.Top + y);
        }
    }

    // 프레임 사각형 영역을 투명하게 지운다(DISPOSE_BACKGROUND).
    void clearRect(std::vector<uint8_t>& canvas, int canvasW, int canvasH,
                   const GifImageDesc& d) {
        for (int y = 0; y < d.Height; ++y) {
            int dstRow = d.Top + y;
            if (dstRow < 0 || dstRow >= canvasH) {
                continue;
            }
            for (int x = 0; x < d.Width; ++x) {
                int px = d.Left + x;
                if (px < 0 || px >= canvasW) {
                    continue;
                }
                uint8_t* out = &canvas[((size_t)dstRow * canvasW + px) * 4];
                out[0] = out[1] = out[2] = out[3] = 0;
            }
        }
    }
} // namespace

DecodedImage decode_gif(const std::string& path) {
    DecodedImage img;

    int err = 0;
    GifFileType* gif = DGifOpenFileName(path.c_str(), &err);
    if (!gif) {
        img.error = std::string("GIF 열기 실패: ") + GifErrorString(err);
        return img;
    }
    if (DGifSlurp(gif) != GIF_OK) {
        img.error = std::string("GIF 파싱 실패: ") + GifErrorString(gif->Error);
        DGifCloseFile(gif, &err);
        return img;
    }
    if (gif->ImageCount < 1) {
        img.error = "GIF 에 이미지가 없습니다";
        DGifCloseFile(gif, &err);
        return img;
    }

    const int W = gif->SWidth;
    const int H = gif->SHeight;
    if (W <= 0 || H <= 0) {
        img.error = "유효하지 않은 GIF 크기입니다";
        DGifCloseFile(gif, &err);
        return img;
    }

    img.width = W;
    img.height = H;

    std::vector<uint8_t> canvas((size_t)W * H * 4, 0);   // 투명으로 초기화
    std::vector<uint8_t> saved;                          // DISPOSE_PREVIOUS 복원용

    for (int i = 0; i < gif->ImageCount; ++i) {
        const SavedImage& frame = gif->SavedImages[i];
        const ColorMapObject* cmap =
            frame.ImageDesc.ColorMap ? frame.ImageDesc.ColorMap : gif->SColorMap;

        // 그래픽 제어 확장(지연/투명/disposal)
        GraphicsControlBlock gcb;
        int transparent = NO_TRANSPARENT_COLOR;
        int disposal = DISPOSAL_UNSPECIFIED;
        int delayCs = 0;
        if (DGifSavedExtensionToGCB(gif, i, &gcb) == GIF_OK) {
            transparent = gcb.TransparentColor;
            disposal = gcb.DisposalMode;
            delayCs = gcb.DelayTime;
        }

        // 이 프레임 이후 DISPOSE_PREVIOUS 를 위해 현재 캔버스 저장
        if (disposal == DISPOSE_PREVIOUS) {
            saved = canvas;
        }

        // 프레임을 캔버스에 합성
        drawFrame(canvas, W, H, frame, cmap, transparent);

        // 스냅샷 저장(브라우저 관례: 지연 0~10ms 는 100ms 로 보정)
        ImageFrame outFrame;
        outFrame.pixels = canvas;
        outFrame.delay_ms = delayCs * 10;
        if (outFrame.delay_ms <= 10) {
            outFrame.delay_ms = 100;
        }
        img.frames.push_back(std::move(outFrame));

        // 다음 프레임을 위한 disposal 처리
        if (disposal == DISPOSE_BACKGROUND) {
            clearRect(canvas, W, H, frame.ImageDesc);
        } else if (disposal == DISPOSE_PREVIOUS && !saved.empty()) {
            canvas = saved;
        }
        // DISPOSE_DO_NOT / UNSPECIFIED: 캔버스 유지
    }

    DGifCloseFile(gif, &err);

    if (img.frames.empty()) {
        img.error = "GIF 디코딩 실패";
        return img;
    }

    img.pixels = img.frames.front().pixels;   // 첫 프레임(정적 표시/폴백용)
    img.ok = true;
    return img;
}
