#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <decoder/image_decoder.h>

#include "imshow.h"

namespace {

constexpr wchar_t window_class_name[] = L"FreeImageViewerImshowWindow";
constexpr UINT_PTR animation_timer_id = 1;
constexpr int minimum_frame_delay_ms = 10;

struct DisplayFrame {
    std::vector<std::uint8_t> bgra;
    UINT delay_ms = minimum_frame_delay_ms;
};

struct WindowState {
    int image_width = 0;
    int image_height = 0;
    std::vector<DisplayFrame> frames;
    std::size_t frame_index = 0;
    BITMAPINFO bitmap_info{};
};

std::size_t required_pixel_bytes(const int width, const int height) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("imshow: invalid image dimensions");
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h / 4) {
        throw std::runtime_error("imshow: image dimensions are too large");
    }
    return w * h * 4;
}

DisplayFrame make_display_frame(const std::vector<std::uint8_t>& rgba,
                                const std::size_t byte_count,
                                const int delay_ms) {
    if (rgba.size() < byte_count) {
        throw std::runtime_error("imshow: decoded pixel buffer is too small");
    }

    DisplayFrame result;
    result.bgra.resize(byte_count);
    for (std::size_t i = 0; i < byte_count; i += 4) {
        result.bgra[i] = rgba[i + 2];
        result.bgra[i + 1] = rgba[i + 1];
        result.bgra[i + 2] = rgba[i];
        result.bgra[i + 3] = rgba[i + 3];
    }
    result.delay_ms = static_cast<UINT>(std::max(delay_ms, minimum_frame_delay_ms));
    return result;
}

WindowState make_window_state(const DecodedImage& decoded) {
    WindowState state;
    state.image_width = decoded.width;
    state.image_height = decoded.height;
    const std::size_t byte_count = required_pixel_bytes(decoded.width, decoded.height);

    if (decoded.animated()) {
        state.frames.reserve(decoded.frames.size());
        for (const ImageFrame& frame : decoded.frames) {
            state.frames.push_back(make_display_frame(frame.pixels, byte_count, frame.delay_ms));
        }
    } else {
        state.frames.push_back(make_display_frame(decoded.pixels, byte_count,
                                                  minimum_frame_delay_ms));
    }

    state.bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    state.bitmap_info.bmiHeader.biWidth = decoded.width;
    // 음수 높이는 픽셀 버퍼가 위에서 아래 방향임을 뜻한다.
    state.bitmap_info.bmiHeader.biHeight = -decoded.height;
    state.bitmap_info.bmiHeader.biPlanes = 1;
    state.bitmap_info.bmiHeader.biBitCount = 32;
    state.bitmap_info.bmiHeader.biCompression = BI_RGB;
    return state;
}

std::wstring utf8_to_wide(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                           nullptr, 0);
    if (length <= 0) {
        return L"FreeImageViewer";
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                        result.data(), length);
    result.pop_back();

    const std::size_t separator = result.find_last_of(L"/\\");
    if (separator != std::wstring::npos) {
        result.erase(0, separator + 1);
    }
    return result.empty() ? L"FreeImageViewer" : result;
}

void draw_image(HWND window, WindowState& state) {
    PAINTSTRUCT paint{};
    HDC device_context = BeginPaint(window, &paint);

    RECT client{};
    GetClientRect(window, &client);
    const int client_width = client.right - client.left;
    const int client_height = client.bottom - client.top;
    FillRect(device_context, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    if (client_width > 0 && client_height > 0 && !state.frames.empty()) {
        int draw_width = client_width;
        int draw_height = static_cast<int>(
                static_cast<long long>(client_width) * state.image_height /
                state.image_width);
        if (draw_height > client_height) {
            draw_height = client_height;
            draw_width = static_cast<int>(
                    static_cast<long long>(client_height) * state.image_width /
                    state.image_height);
        }

        const int x = (client_width - draw_width) / 2;
        const int y = (client_height - draw_height) / 2;
        const DisplayFrame& frame = state.frames[state.frame_index];
        SetStretchBltMode(device_context, HALFTONE);
        SetBrushOrgEx(device_context, 0, 0, nullptr);
        StretchDIBits(device_context,
                      x, y, draw_width, draw_height,
                      0, 0, state.image_width, state.image_height,
                      frame.bgra.data(), &state.bitmap_info,
                      DIB_RGB_COLORS, SRCCOPY);
    }

    EndPaint(window, &paint);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message,
                                  WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* state = reinterpret_cast<WindowState*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_KEYDOWN:
            if (w_param == VK_SPACE) {
                DestroyWindow(window);
                return 0;
            }
            break;

        case WM_TIMER:
            if (w_param == animation_timer_id && state != nullptr &&
                state->frames.size() > 1) {
                KillTimer(window, animation_timer_id);
                state->frame_index = (state->frame_index + 1) % state->frames.size();
                InvalidateRect(window, nullptr, FALSE);
                SetTimer(window, animation_timer_id,
                         state->frames[state->frame_index].delay_ms, nullptr);
                return 0;
            }
            break;

        case WM_PAINT:
            if (state != nullptr) {
                draw_image(window, *state);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            KillTimer(window, animation_timer_id);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void register_window_class(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = window_class_name;

    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("imshow: failed to register the Win32 window class");
    }
}

} // namespace

void imshow(const char* image_path) {
    if (image_path == nullptr || image_path[0] == '\0') {
        throw std::invalid_argument("imshow: image_path is empty");
    }

    const DecodedImage decoded = decode_image(std::string(image_path));
    if (!decoded.ok) {
        throw std::runtime_error("imshow: " + decoded.error);
    }
    WindowState state = make_window_state(decoded);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    register_window_class(instance);

    constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;
    RECT window_rect{0, 0, decoded.width, decoded.height};
    if (!AdjustWindowRectEx(&window_rect, window_style, FALSE, 0)) {
        throw std::runtime_error("imshow: failed to calculate the window size");
    }

    const std::wstring title = utf8_to_wide(image_path);
    HWND window = CreateWindowExW(
            0, window_class_name, title.c_str(), window_style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            window_rect.right - window_rect.left,
            window_rect.bottom - window_rect.top,
            nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        throw std::runtime_error("imshow: failed to create the Win32 window");
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    if (state.frames.size() > 1) {
        SetTimer(window, animation_timer_id, state.frames.front().delay_ms, nullptr);
    }

    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result == -1) {
        if (IsWindow(window)) {
            DestroyWindow(window);
        }
        throw std::runtime_error("imshow: Win32 message loop failed");
    }
}
