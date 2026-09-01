//
// viewer.mm
// Objective-C++ 로 작성한 Cocoa GUI 이미지 뷰어.
// C++(jpeg_decoder)로 JPEG 를 RGBA 로 디코딩한 뒤 NSImageView 에 표시한다.
// NSScrollView 의 매그니피케이션으로 확대/축소(트랙패드 핀치 · 키보드 단축키)를 지원한다.
//
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

#include "image_decoder.h"
#include "viewer.h"

static const CGFloat kZoomStep = 1.25;   // 한 단계 확대/축소 배율
static const CGFloat kMinZoom  = 0.05;   // 최소 5%
static const CGFloat kMaxZoom  = 40.0;   // 최대 4000%
static const int     kPreviewMaxPixel = 512;          // ImageIO 미리보기 최대 변 길이
static const long long kPreviewMinPixels = 1024LL * 1024LL; // 이보다 큰 이미지에만 미리보기
static const NSInteger  kThumbPreloadRadius = 64;     // 현재 이미지 기준 앞뒤로 미리 만들 썸네일 개수
static const NSUInteger kThumbCacheLimit    = 512;    // 썸네일 캐시 최대 항목 수(초과 시 창 밖 항목 제거)

static void ApplyBundleApplicationIcon(NSApplication* app) {
    NSString* iconFile = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleIconFile"];
    if (![iconFile isKindOfClass:[NSString class]] || iconFile.length == 0) {
        return;
    }

    NSString* iconName = [iconFile stringByDeletingPathExtension];
    NSString* iconExtension = iconFile.pathExtension.length > 0 ? iconFile.pathExtension : @"icns";
    NSString* iconPath = [[NSBundle mainBundle] pathForResource:iconName ofType:iconExtension];
    if (!iconPath) {
        iconPath = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:iconFile];
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:iconPath]) {
        NSString* executableDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        NSString* resourcesDir = [[executableDir stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Resources"];
        iconPath = [resourcesDir stringByAppendingPathComponent:iconFile];
    }

    NSImage* icon = [[NSImage alloc] initWithContentsOfFile:iconPath];
    if (icon && icon.isValid) {
        [icon setTemplate:NO];
        [app setApplicationIconImage:icon];
    }
}

// ---------------------------------------------------------------------------
// RGBA 픽셀 버퍼를 NSImage 로 변환한다.
// ---------------------------------------------------------------------------
static NSImage* NSImageFromRGBA(const uint8_t* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) {
        return nil;
    }
    NSBitmapImageRep* rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                pixelsWide:width
                                                pixelsHigh:height
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:width * 4
                                              bitsPerPixel:32];
    if (!rep) {
        return nil;
    }
    // 디코딩된 RGBA 픽셀을 그대로 복사(NSImage 가 자체 버퍼를 소유)
    memcpy([rep bitmapData], pixels, (size_t)width * height * 4);

    NSImage* image =
        [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:rep];
    return image;
}

static NSImage* NSImageFromDecoded(const DecodedImage& img) {
    return NSImageFromRGBA(img.pixels.data(), img.width, img.height);
}

// ---------------------------------------------------------------------------
// ImageIO 로 빠른 축소 썸네일을 만든다(PNG/WebP/TIFF/GIF/AVIF 등 비-JPEG 미리보기용).
// Apple 의 최적화된 디코더가 저해상도 썸네일을 매우 빠르게 생성한다.
// 성공 시 NSImage 를 반환하고 outFullSize 에 원본 픽셀 크기를 담는다.
// ---------------------------------------------------------------------------
// maxPixel 이 0이면 원본 최대 변의 1/8 크기로 만든다(연속 탐색 미리보기용).
static NSImage* FastThumbnail(NSString* path, int maxPixel, NSSize* outFullSize) {
    if (outFullSize) {
        *outFullSize = NSZeroSize;
    }
    NSURL* url = [NSURL fileURLWithPath:path];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (!src) {
        return nil;
    }

    // 원본 픽셀 크기(헤더만 읽어 빠름). EXIF 방향이 90/270°면 가로·세로를 뒤바꾼다.
    int fullW = 0, fullH = 0;
    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
    if (props) {
        int orientation = 1;
        CFNumberRef w = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyPixelWidth);
        CFNumberRef h = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyPixelHeight);
        CFNumberRef o = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyOrientation);
        if (w) CFNumberGetValue(w, kCFNumberIntType, &fullW);
        if (h) CFNumberGetValue(h, kCFNumberIntType, &fullH);
        if (o) CFNumberGetValue(o, kCFNumberIntType, &orientation);
        if (orientation >= 5 && orientation <= 8) {   // 회전 → 방향 반영 크기
            int t = fullW; fullW = fullH; fullH = t;
        }
        if (outFullSize) *outFullSize = NSMakeSize(fullW, fullH);
        CFRelease(props);
    }

    // 축소 썸네일 생성 옵션
    int maxPx = maxPixel;
    if (maxPx == 0) {
        // 최대 변을 1/8로 제한하면 종횡비를 유지한 채 두 변 모두 약 1/8이 된다.
        maxPx = fullW > 0 && fullH > 0 ? MAX(1, (MAX(fullW, fullH) + 7) / 8)
                                       : kPreviewMaxPixel;
    }
    CFNumberRef maxPxNum = CFNumberCreate(NULL, kCFNumberIntType, &maxPx);
    const void* keys[] = {
        kCGImageSourceCreateThumbnailFromImageAlways,
        kCGImageSourceThumbnailMaxPixelSize,
        kCGImageSourceCreateThumbnailWithTransform,
    };
    const void* values[] = { kCFBooleanTrue, maxPxNum, kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, values, 3,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);

    CGImageRef thumb = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);
    CFRelease(maxPxNum);
    CFRelease(opts);
    CFRelease(src);
    if (!thumb) {
        return nil;
    }

    NSImage* image = [[NSImage alloc] initWithCGImage:thumb size:NSZeroSize];
    CGImageRelease(thumb);
    return image;
}

// ---------------------------------------------------------------------------
// ImageIO 가 못 여는 포맷(QOI, 구형 macOS 의 JXL 등)의 썸네일 폴백:
// 자체 C++ 디코더로 전체 디코딩 후 최근접 샘플링으로 축소한다(백그라운드 큐 전용).
// ---------------------------------------------------------------------------
static NSImage* DecoderThumbnail(NSString* path, int maxPixel, NSSize* outFullSize) {
    if (outFullSize) {
        *outFullSize = NSZeroSize;
    }
    DecodedImage full = decode_image(path.UTF8String);
    if (!full.ok || full.width <= 0 || full.height <= 0) {
        return nil;
    }
    const int w = full.width, h = full.height;
    if (outFullSize) {
        *outFullSize = NSMakeSize(w, h);
    }
    if (w <= maxPixel && h <= maxPixel) {
        return NSImageFromRGBA(full.pixels.data(), w, h);
    }
    const double scale = MIN((double)maxPixel / w, (double)maxPixel / h);
    const int tw = MAX(1, (int)lround(w * scale));
    const int th = MAX(1, (int)lround(h * scale));
    std::vector<uint8_t> out((size_t)tw * th * 4);
    for (int y = 0; y < th; y++) {
        const int sy = MIN(h - 1, (int)(((int64_t)y * 2 + 1) * h / (th * 2)));
        const uint8_t* srow = full.pixels.data() + (size_t)sy * w * 4;
        uint8_t* drow = out.data() + (size_t)y * tw * 4;
        for (int x = 0; x < tw; x++) {
            const int sx = MIN(w - 1, (int)(((int64_t)x * 2 + 1) * w / (tw * 2)));
            memcpy(drow + (size_t)x * 4, srow + (size_t)sx * 4, 4);
        }
    }
    return NSImageFromRGBA(out.data(), tw, th);
}

// ---------------------------------------------------------------------------
// ImageIO/NSImage 로 전체 해상도를 디코딩한다(HEIC/HEIF 등 C 라이브러리로 처리하지
// 않는 포맷의 폴백). EXIF 방향을 적용하고, outPixelSize 에 결과 픽셀 크기를 담는다.
// ---------------------------------------------------------------------------
static NSImage* DecodeNativeFullImage(NSString* path, NSSize* outPixelSize) {
    if (outPixelSize) {
        *outPixelSize = NSZeroSize;
    }
    NSURL* url = [NSURL fileURLWithPath:path];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (!src) {
        return nil;
    }

    // 원본 픽셀 크기(방향 적용을 위해 최대 변 길이로 썸네일 요청)
    int w = 0, h = 0;
    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
    if (props) {
        CFNumberRef wn = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyPixelWidth);
        CFNumberRef hn = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyPixelHeight);
        if (wn) CFNumberGetValue(wn, kCFNumberIntType, &w);
        if (hn) CFNumberGetValue(hn, kCFNumberIntType, &h);
        CFRelease(props);
    }

    CGImageRef cg = NULL;
    const int maxDim = (w > h) ? w : h;
    if (maxDim > 0) {
        // WithTransform=YES 로 EXIF 방향을 적용한 전체 해상도 이미지를 얻는다.
        int mp = maxDim;
        CFNumberRef mpNum = CFNumberCreate(NULL, kCFNumberIntType, &mp);
        const void* keys[] = {
            kCGImageSourceCreateThumbnailFromImageAlways,
            kCGImageSourceThumbnailMaxPixelSize,
            kCGImageSourceCreateThumbnailWithTransform,
        };
        const void* values[] = { kCFBooleanTrue, mpNum, kCFBooleanTrue };
        CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, values, 3,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
        cg = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);
        CFRelease(mpNum);
        CFRelease(opts);
    }
    if (!cg) {
        cg = CGImageSourceCreateImageAtIndex(src, 0, NULL);   // 폴백(방향 미적용)
    }
    CFRelease(src);
    if (!cg) {
        return nil;
    }

    if (outPixelSize) {
        *outPixelSize = NSMakeSize(CGImageGetWidth(cg), CGImageGetHeight(cg));
    }
    NSImage* image = [[NSImage alloc] initWithCGImage:cg size:NSZeroSize];
    CGImageRelease(cg);
    return image;
}

// ---------------------------------------------------------------------------
// 문서(이미지)가 뷰포트보다 작을 때 중앙에 오도록 하는 클립 뷰.
// (기본 NSClipView 는 좌측 하단에 정렬한다)
// ---------------------------------------------------------------------------
@interface CenteringClipView : NSClipView
@end

@implementation CenteringClipView
- (NSRect)constrainBoundsRect:(NSRect)proposedBounds {
    NSRect rect = [super constrainBoundsRect:proposedBounds];
    NSView* docView = self.documentView;
    if (docView) {
        // rect 는 매그니피케이션이 반영된 문서 좌표계 기준.
        NSSize docSize = docView.frame.size;
        if (rect.size.width > docSize.width) {
            rect.origin.x = (docSize.width - rect.size.width) / 2.0;
        }
        if (rect.size.height > docSize.height) {
            rect.origin.y = (docSize.height - rect.size.height) / 2.0;
        }
    }
    return rect;
}
@end

// ---------------------------------------------------------------------------
// 앱 델리게이트
// ---------------------------------------------------------------------------
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSScrollView* scrollView;
@property(nonatomic, strong) NSImageView* imageView;
@property(nonatomic, copy) NSString* startupPath;   // 커맨드라인으로 넘어온 경로(있으면)
@property(nonatomic) BOOL fitToWindow;              // 기본 상태(창 리사이즈 시 자동 맞춤) 여부

// --- GIF 등 애니메이션 재생 상태 ---
@property(nonatomic, strong) NSArray<NSImage*>* animFrames;   // 프레임 이미지들
@property(nonatomic, strong) NSArray<NSNumber*>* animDelays;  // 프레임별 지연(초)
@property(nonatomic) NSInteger animIndex;                     // 현재 프레임
@property(nonatomic, strong) NSTimer* animTimer;

@property(nonatomic) NSUInteger loadGeneration;              // 로드 요청 세대(오래된 백그라운드 결과 무시용)

// --- 같은 디렉토리 내 이미지 목록 탐색(방향키) ---
@property(nonatomic, strong) NSArray<NSString*>* directoryImages;  // 정렬된 절대 경로 목록
@property(nonatomic) NSInteger currentIndex;                       // directoryImages 내 현재 위치

// --- 방향키 꾹 누름 시 고속 반복 전환 ---
@property(nonatomic, strong) NSTimer* keyRepeatTimer;   // 자체 반복 타이머(시스템 키 반복 대신)
@property(nonatomic, strong) NSTimer* thumbnailModeTimer; // 1초 후 썸네일 전용 모드 전환
@property(nonatomic) unsigned short heldArrowKeyCode;   // 현재 누르고 있는 방향키(123/124), 없으면 0
@property(nonatomic) BOOL arrowThumbnailOnly;           // 길게 누르는 동안 원본 로드 생략
@property(nonatomic) BOOL currentImageHasPreview;       // 현재 경로의 저해상도 화면 표시 여부
@property(nonatomic, strong) id captionDoubleClickMonitor; // 상단 캡션 영역 더블클릭 확대

// --- 썸네일 프리로드 캐시(방향키 탐색을 빠르고 부드럽게) ---
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSImage*>* thumbCache;    // 경로 → 썸네일
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSValue*>* thumbSizeCache;// 경로 → 원본 픽셀 크기
@property(nonatomic, strong) NSMutableSet<NSString*>* thumbInFlight; // 생성 중인 경로(중복 작업 방지)
@property(nonatomic, strong) NSOperationQueue* thumbQueue;           // 코어 절반만큼 병렬 썸네일 생성
@end

@implementation AppDelegate

// File > Open... : 파일 선택 대화상자로 JPEG 를 골라 연다.
- (void)openDocument:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    // UTType 기반 필터(allowedContentTypes): 폴더 탐색은 그대로 두고 파일만 걸러진다.
    // (구식 allowedFileTypes 는 최신 macOS 에서 디렉토리까지 가려 보이는 문제가 있음)
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (NSString* ext in @[ @"jpg", @"jpeg", @"png", @"webp", @"gif",
                             @"tif", @"tiff", @"avif", @"heic", @"heif", @"qoi",
                             @"jxl", @"bmp", @"tga", @"pbm", @"pgm", @"pnm",
                             @"ppm" ]) {
        UTType* t = [UTType typeWithFilenameExtension:ext];
        if (t) {
            [types addObject:t];
        }
    }
    panel.allowedContentTypes = types;
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.message = @"열어볼 이미지를 선택하세요 (JPEG/PNG/WebP/GIF/TIFF/AVIF/HEIC/QOI/JXL/BMP/TGA/PNM)";
    if ([panel runModal] == NSModalResponseOK) {
        [self showImageAtPath:panel.URLs.firstObject.path];
    }
}

// 오류를 알린다(앱은 종료하지 않고 계속 열기 가능).
- (void)failWithMessage:(NSString*)message {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"이미지를 열 수 없습니다";
    alert.informativeText = message;
    [alert addButtonWithTitle:@"확인"];
    [alert runModal];
}

// ------------------------- 확대/축소 --------------------------

// fit 상태에서는 스크롤할 것이 없으므로 스크롤바를 아예 끈다.
// (fit 은 한 축을 창에 정확히 채우는데, 서브픽셀 반올림 오차만으로도
//  autohidesScrollers 가 스크롤바를 띄워 리사이즈 중 깜빡이기 때문)
- (void)setFitToWindow:(BOOL)fitToWindow {
    _fitToWindow = fitToWindow;
    self.scrollView.hasVerticalScroller = !fitToWindow;
    self.scrollView.hasHorizontalScroller = !fitToWindow;
}

// 현재 보이는 영역의 중심을 기준으로 배율을 적용한다(범위 제한 포함).
- (void)applyMagnification:(CGFloat)mag {
    // 아주 큰 이미지의 fit 배율이 기본 수동 축소 한도(5%)보다 작을 수 있다.
    // fit 상태에서는 그 배율까지 허용해 이미지가 반드시 창 안에 들어오게 한다.
    CGFloat minMag = self.fitToWindow ? MIN(kMinZoom, mag) : kMinZoom;
    self.scrollView.minMagnification = minMag;
    mag = MAX(minMag, MIN(kMaxZoom, mag));
    NSRect vis = self.scrollView.documentVisibleRect;
    NSPoint center = NSMakePoint(NSMidX(vis), NSMidY(vis));
    [self.scrollView setMagnification:mag centeredAtPoint:center];
    [self updateWindowTitle];
}

// 뷰포트에 이미지가 딱 맞는 배율.
- (CGFloat)fitMagnification {
    NSSize viewport = self.scrollView.contentView.frame.size;   // 매그니피케이션과 무관한 실제 표시 영역
    NSSize img = self.imageView.frame.size;
    if (img.width <= 0 || img.height <= 0) {
        return 1.0;
    }
    return MIN(viewport.width / img.width, viewport.height / img.height);
}

// 기본 상태 배율: 창에 맞춘다(이미지가 창보다 작으면 확대해서라도 채움).
// 이 fit 배율이 표시상 100% 기준이 된다.
- (CGFloat)defaultMagnification {
    return [self fitMagnification];
}

- (void)zoomIn:(id)sender {
    self.fitToWindow = NO;   // 수동 확대 → 이후 리사이즈에서 배율 유지
    [self applyMagnification:self.scrollView.magnification * kZoomStep];
}

- (void)zoomOut:(id)sender {
    self.fitToWindow = NO;   // 수동 축소 → 이후 리사이즈에서 배율 유지
    [self applyMagnification:self.scrollView.magnification / kZoomStep];
}

- (void)zoomActualSize:(id)sender {
    self.fitToWindow = NO;
    [self applyMagnification:1.0];   // 원본 픽셀 1:1 (표시 퍼센트는 fit 기준으로 환산됨)
}

// Cmd+0: 기본 상태(창에 맞춤, 100% 이내)로 되돌린다. 이후 창 리사이즈 시 자동 맞춤.
- (void)resetToDefault:(id)sender {
    self.fitToWindow = YES;
    [self applyMagnification:[self defaultMagnification]];
}

// 트랙패드 핀치로 확대/축소가 시작되면 기본 상태 해제.
- (void)liveMagnifyStarted:(NSNotification*)note {
    self.fitToWindow = NO;
}

- (void)updateWindowTitle {
    if (!self.imageView.image) {
        return;
    }
    NSSize px = self.imageView.frame.size;
    // 퍼센트는 원본 픽셀이 아니라 "현재 창에 fit 한 배율"을 100% 로 삼아 표시한다.
    // (창을 리사이즈해도 fit 상태면 항상 100%)
    CGFloat fit = [self fitMagnification];
    if (fit <= 0) {
        fit = 1.0;
    }
    int pct = (int)round(self.scrollView.magnification / fit * 100.0);
    self.window.title = [NSString stringWithFormat:@"%@  (%dx%d, %d%%)",
                                                    self.window.representedFilename.lastPathComponent
                                                        ?: @"image",
                                                    (int)px.width, (int)px.height, pct];
}

// 창 크기가 바뀔 때: 기본 상태면 새 크기에 맞춰 이미지를 다시 fit 한다.
- (void)windowDidResize:(NSNotification*)note {
    if (self.fitToWindow && self.imageView.image) {
        [self applyMagnification:[self defaultMagnification]];
    } else {
        [self updateWindowTitle];
    }
}

// ------------------------- 메뉴 구성 --------------------------

- (void)buildMainMenu {
    NSMenu* mainMenu = [[NSMenu alloc] init];

    // 앱 메뉴 (Quit)
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit Image Viewer"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    // File 메뉴 (열기)
    NSMenuItem* fileItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:fileItem];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem* open = [fileMenu addItemWithTitle:@"Open…"
                                           action:@selector(openDocument:)
                                    keyEquivalent:@"o"];
    open.target = self;
    [fileMenu addItemWithTitle:@"Close"
                        action:@selector(performClose:)
                 keyEquivalent:@"w"];
    fileItem.submenu = fileMenu;

    // View 메뉴 (확대/축소)
    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];

    // ⌘+  (물리적으로 Cmd-Shift-= 를 잡는다)
    NSMenuItem* zin = [viewMenu addItemWithTitle:@"Zoom In"
                                          action:@selector(zoomIn:)
                                   keyEquivalent:@"+"];
    zin.target = self;
    // ⌘=  (Shift 없이 누르는 경우도 지원)
    NSMenuItem* zinAlt = [viewMenu addItemWithTitle:@"Zoom In (=)"
                                             action:@selector(zoomIn:)
                                      keyEquivalent:@"="];
    zinAlt.target = self;
    zinAlt.keyEquivalentModifierMask = NSEventModifierFlagCommand;

    NSMenuItem* zout = [viewMenu addItemWithTitle:@"Zoom Out"
                                           action:@selector(zoomOut:)
                                    keyEquivalent:@"-"];
    zout.target = self;

    [viewMenu addItem:[NSMenuItem separatorItem]];

    // ⌘0: 기본 상태(창에 맞춤) — 리사이즈 시 자동 맞춤
    NSMenuItem* fit = [viewMenu addItemWithTitle:@"Fit to Window (Default)"
                                          action:@selector(resetToDefault:)
                                   keyEquivalent:@"0"];
    fit.target = self;

    // ⌘1: 원본 픽셀 1:1
    NSMenuItem* actual = [viewMenu addItemWithTitle:@"Actual Size (1:1)"
                                             action:@selector(zoomActualSize:)
                                      keyEquivalent:@"1"];
    actual.target = self;

    viewItem.submenu = viewMenu;

    // Go 메뉴 (이전/다음 이미지) — 방향키 ←/→ 로도 넘길 수 있음
    NSMenuItem* goItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:goItem];
    NSMenu* goMenu = [[NSMenu alloc] initWithTitle:@"Go"];

    NSString* leftArrow  = [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey];
    NSString* rightArrow = [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey];

    NSMenuItem* prev = [goMenu addItemWithTitle:@"Previous Image"
                                         action:@selector(showPreviousImage:)
                                  keyEquivalent:leftArrow];
    prev.target = self;
    prev.keyEquivalentModifierMask = NSEventModifierFlagCommand;   // ⌘←

    NSMenuItem* next = [goMenu addItemWithTitle:@"Next Image"
                                         action:@selector(showNextImage:)
                                  keyEquivalent:rightArrow];
    next.target = self;
    next.keyEquivalentModifierMask = NSEventModifierFlagCommand;   // ⌘→

    goItem.submenu = goMenu;

    [NSApp setMainMenu:mainMenu];
}

// ------------------------- 실행 --------------------------

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    self.thumbCache = [NSMutableDictionary dictionary];
    self.thumbSizeCache = [NSMutableDictionary dictionary];
    self.thumbInFlight = [NSMutableSet set];
    self.thumbQueue = [[NSOperationQueue alloc] init];
    self.thumbQueue.name = @"viewer.thumbnail.preload";
    self.thumbQueue.qualityOfService = NSQualityOfServiceUserInitiated;
    // 곧 넘겨 볼 이미지의 캐시는 사용자 체감 작업이다. 논리 CPU 코어의 절반만
    // 사용해 UI/원본 디코딩 여유를 남기면서 직렬 프리로드 병목을 없앤다.
    const NSInteger processorCount =
        MAX(1, (NSInteger)NSProcessInfo.processInfo.activeProcessorCount);
    self.thumbQueue.maxConcurrentOperationCount = MAX(1, processorCount / 2);

    [self buildMainMenu];
    [self setupWindow];
    [self installArrowKeyNavigation];
    [self installCaptionDoubleClickZoom];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // 커맨드라인 인자로 경로가 주어졌으면 연다(없으면 빈 창 — File ▸ Open ⌘O 로 열기).
    if (self.startupPath.length > 0 &&
        [[NSFileManager defaultManager] fileExistsAtPath:self.startupPath]) {
        [self showImageAtPath:self.startupPath];
    }
}

// 빈 창(이미지 없는 상태)을 만든다. 이후 showImageAtPath: 로 내용을 채운다.
- (void)setupWindow {
    NSRect winFrame = NSMakeRect(0, 0, 720, 540);

    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:winFrame
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskMiniaturizable |
                                               NSWindowStyleMaskResizable |
                                               NSWindowStyleMaskFullSizeContentView)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.title = @"Image Viewer — File ▸ Open (⌘O)";
    window.titleVisibility = NSWindowTitleHidden;
    window.titlebarAppearsTransparent = YES;
    window.movableByWindowBackground = YES;
    if (@available(macOS 11.0, *)) {
        window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    }
    window.delegate = (id<NSWindowDelegate>)self;

    // === 이미지 뷰: 프레임을 원본 크기로 두고 매그니피케이션으로 확대/축소 ===
    NSImageView* imageView = [[NSImageView alloc] initWithFrame:winFrame];
    imageView.imageScaling = NSImageScaleAxesIndependently;   // 프레임을 꽉 채우도록
    imageView.imageAlignment = NSImageAlignCenter;
    imageView.editable = NO;

    // === 스크롤 뷰: 확대/축소 + 패닝 지원 ===
    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:winFrame];
    scrollView.hasVerticalScroller = YES;
    scrollView.hasHorizontalScroller = YES;
    scrollView.autohidesScrollers = YES;
    scrollView.allowsMagnification = YES;          // 트랙패드 핀치 줌 활성화
    scrollView.minMagnification = kMinZoom;
    scrollView.maxMagnification = kMaxZoom;
    // 축소 시 이미지가 창 중앙에 오도록 중앙 정렬 클립 뷰 사용
    scrollView.contentView = [[CenteringClipView alloc] initWithFrame:winFrame];
    scrollView.documentView = imageView;
    scrollView.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);
    scrollView.borderType = NSNoBorder;
    scrollView.backgroundColor = [NSColor windowBackgroundColor];

    window.contentView = scrollView;

    // 트랙패드 핀치 줌 시작을 감지해 기본 상태를 해제한다.
    [[NSNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(liveMagnifyStarted:)
               name:NSScrollViewWillStartLiveMagnifyNotification
             object:scrollView];

    self.window = window;
    self.scrollView = scrollView;
    self.imageView = imageView;
}

- (CGFloat)captionDoubleClickHeight {
    CGFloat height = NSHeight(self.window.contentView.bounds) - NSHeight(self.window.contentLayoutRect);
    if (height < 24.0 || height > 80.0) {
        height = 36.0;
    }
    return height;
}

- (BOOL)pointIsInStandardWindowButton:(NSPoint)point {
    const NSWindowButton buttonTypes[] = {
        NSWindowCloseButton,
        NSWindowMiniaturizeButton,
        NSWindowZoomButton,
    };
    for (NSWindowButton buttonType : buttonTypes) {
        NSButton* button = [self.window standardWindowButton:buttonType];
        if (!button || button.isHidden || !button.superview) {
            continue;
        }
        NSRect buttonFrame = [button.superview convertRect:button.frame toView:nil];
        if (NSPointInRect(point, NSInsetRect(buttonFrame, -6.0, -6.0))) {
            return YES;
        }
    }
    return NO;
}

- (void)installCaptionDoubleClickZoom {
    __weak AppDelegate* weakSelf = self;
    self.captionDoubleClickMonitor =
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                              handler:^NSEvent* (NSEvent* event) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf || event.window != strongSelf.window || event.clickCount != 2) {
                return event;
            }

            NSPoint point = event.locationInWindow;
            CGFloat topEdge = NSHeight(strongSelf.window.contentView.bounds) -
                              [strongSelf captionDoubleClickHeight];
            if (point.y >= topEdge && ![strongSelf pointIsInStandardWindowButton:point]) {
                [strongSelf.window performZoom:nil];
                return nil;
            }
            return event;
        }];
}

// GIF 등 애니메이션 재생 제어 ------------------------------------------------

- (void)stopAnimation {
    [self.animTimer invalidate];
    self.animTimer = nil;
    self.animFrames = nil;
    self.animDelays = nil;
    self.animIndex = 0;
}

// 디코딩된 프레임들로 애니메이션을 시작한다.
- (void)startAnimationWithDecoded:(const DecodedImage&)decoded {
    NSMutableArray<NSImage*>* frames = [NSMutableArray arrayWithCapacity:decoded.frames.size()];
    NSMutableArray<NSNumber*>* delays = [NSMutableArray arrayWithCapacity:decoded.frames.size()];
    for (const ImageFrame& f : decoded.frames) {
        NSImage* im = NSImageFromRGBA(f.pixels.data(), decoded.width, decoded.height);
        if (!im) {
            continue;
        }
        [frames addObject:im];
        [delays addObject:@(f.delay_ms / 1000.0)];   // ms → 초
    }
    self.animFrames = frames;
    self.animDelays = delays;
    self.animIndex = 0;
    self.imageView.image = frames.firstObject;
    [self scheduleNextFrameAfter:delays.firstObject.doubleValue];
}

- (void)scheduleNextFrameAfter:(NSTimeInterval)delay {
    if (delay < 0.02) {
        delay = 0.1;   // 지연 0(또는 비정상적으로 짧음)인 GIF 는 관례상 100ms 로 취급
    }
    [self.animTimer invalidate];
    self.animTimer = [NSTimer scheduledTimerWithTimeInterval:delay
                                                      target:self
                                                    selector:@selector(tickAnimation:)
                                                    userInfo:nil
                                                     repeats:NO];
}

- (void)tickAnimation:(NSTimer*)timer {
    if (self.animFrames.count == 0) {
        return;
    }
    self.animIndex = (self.animIndex + 1) % (NSInteger)self.animFrames.count;
    self.imageView.image = self.animFrames[self.animIndex];
    [self scheduleNextFrameAfter:self.animDelays[self.animIndex].doubleValue];
}

// 새 이미지를 화면에 반영할 때 창 크기는 그대로 유지하고,
// 원본 종횡비를 유지한 채 현재 앱의 가용 영역 안에 맞춘다.
- (void)fitImageToWindowForPath:(NSString*)path {
    self.window.representedFilename = path;
    self.fitToWindow = YES;
    [self applyMagnification:[self defaultMagnification]];
}

// 현재 화면을 다시 준비하지 않고 원본 디코딩만 즉시 요청한다. 방향키를 한 번
// 탭하고 놓았을 때 디렉터리 스캔과 미리보기 디코딩을 반복하지 않기 위한 경로다.
- (void)loadFullResolutionAtPath:(NSString*)path hadPreview:(BOOL)hadPreview {
    path = [path copy];
    const NSUInteger gen = ++self.loadGeneration;
    const std::string p(path.UTF8String);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto full = std::make_shared<DecodedImage>(decode_image(p));

        // C 라이브러리가 처리하지 못하는 포맷(HEIC/HEIF 등)은 NSImage 로 폴백
        NSImage* nativeImage = nil;
        NSSize nativeSize = NSZeroSize;
        if (!full->ok) {
            nativeImage = DecodeNativeFullImage(path, &nativeSize);
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            if (gen != self.loadGeneration) {
                return;   // 그 사이 다른 이미지를 열었음 → 오래된 결과 폐기
            }

            if (full->ok) {
                // === C++ 디코더 경로(JPEG/PNG/WebP/GIF/TIFF/AVIF) ===
                self.imageView.frame = NSMakeRect(0, 0, full->width, full->height);
                if (full->animated()) {
                    [self startAnimationWithDecoded:*full];   // GIF/AVIF 애니메이션 재생
                } else {
                    self.imageView.image = NSImageFromDecoded(*full);   // 선명한 전체 해상도로 교체
                }
                if (!hadPreview) {
                    [self fitImageToWindowForPath:path];
                }
                self.currentImageHasPreview = YES;
                [self updateWindowTitle];
            } else if (nativeImage && nativeSize.width >= 1 && nativeSize.height >= 1) {
                // === NSImage 네이티브 경로(HEIC/HEIF 등) ===
                const int w = (int)nativeSize.width;
                const int h = (int)nativeSize.height;
                self.imageView.frame = NSMakeRect(0, 0, w, h);
                self.imageView.image = nativeImage;
                if (!hadPreview) {
                    [self fitImageToWindowForPath:path];
                }
                self.currentImageHasPreview = YES;
                [self updateWindowTitle];
            } else if (!hadPreview) {
                self.currentImageHasPreview = NO;
                [self failWithMessage:[NSString stringWithUTF8String:full->error.c_str()]];
            }
        });
    });
}

// File ▸ Open, 시작 인자, 방향키 탐색 모두 현재 창 크기를 유지한다.
- (void)showImageAtPath:(NSString*)path {
    // 1) JPEG 큰 이미지는 1/8 저해상도 미리보기를 즉시 띄우고,
    // 2) 전체 해상도는 백그라운드에서 디코딩해 완료되면 교체한다.
    // path 가 directoryImages 의 원소일 수 있으므로, 아래에서 목록을 교체하기 전에
    // 소유권을 확보해 둔다(교체 시 기존 배열과 함께 해제되는 것을 방지).
    path = [path copy];
    const BOOL previewOnly = self.arrowThumbnailOnly;

    ++self.loadGeneration;   // 이전 경로의 원본 디코딩 결과를 무효화
    [self stopAnimation];

    // 같은 디렉토리의 이미지 목록을 갱신하고 현재 위치를 찾는다(방향키 탐색용).
    [self updateDirectoryListForPath:path];

    // 주변 이미지들의 썸네일을 백그라운드에서 미리 만들어 둔다(다음 탐색 대비).
    [self preloadThumbnailsAroundCurrentIndex];

    const std::string p(path.UTF8String);

    // 1) 빠른 저해상도 미리보기 — 있으면 즉시 표시
    BOOL hadPreview = NO;

    //   (0) 프리로드된 썸네일 캐시 — 디코딩 없이 즉시 표시(방향키 연타에도 부드러움)
    NSString* cacheKey = [NSURL fileURLWithPath:path].path;
    NSImage* cachedThumb = self.thumbCache[cacheKey];
    NSValue* cachedSize = self.thumbSizeCache[cacheKey];
    if (cachedThumb && cachedSize) {
        const NSSize fullSize = cachedSize.sizeValue;
        const int fullW = (int)fullSize.width;
        const int fullH = (int)fullSize.height;
        self.imageView.frame = NSMakeRect(0, 0, fullW, fullH);
        self.imageView.image = cachedThumb;   // 썸네일을 원본 크기 프레임에 채워 그림
        [self fitImageToWindowForPath:path];
        [self updateWindowTitle];
        hadPreview = YES;
    }

    //   (a) JPEG: libjpeg-turbo 의 1/8 DCT 스케일 디코딩(가장 빠름)
    DecodedImage preview;
    if (!hadPreview) {
        // 방향키를 누르는 동안에는 작은 JPEG 도 반드시 1/8 DCT 미리보기를 사용한다.
        preview = decode_preview(p, previewOnly);
    }
    if (preview.ok) {
        const int fullW = preview.fullWidth  > 0 ? preview.fullWidth  : preview.width;
        const int fullH = preview.fullHeight > 0 ? preview.fullHeight : preview.height;
        self.imageView.frame = NSMakeRect(0, 0, fullW, fullH);
        // 저해상도 이미지를 원본 크기 프레임에 채워 그림(부드럽게 확대된 미리보기)
        self.imageView.image = NSImageFromRGBA(preview.pixels.data(),
                                               preview.width, preview.height);
        [self fitImageToWindowForPath:path];
        [self updateWindowTitle];
        hadPreview = YES;
    } else if (!hadPreview) {
        //   (b) 그 외 포맷(PNG/WebP/TIFF/GIF/AVIF): ImageIO 축소 썸네일
        NSSize fullSize = NSZeroSize;
        NSImage* thumb = FastThumbnail(path, previewOnly ? 0 : kPreviewMaxPixel, &fullSize);
        if (thumb && fullSize.width >= 1 && fullSize.height >= 1) {
            [self storeThumbnail:thumb size:fullSize forPath:cacheKey];   // 다음에 되돌아올 때 재사용
            if (previewOnly ||
                (long long)fullSize.width * (long long)fullSize.height >= kPreviewMinPixels) {
                const int fullW = (int)fullSize.width;
                const int fullH = (int)fullSize.height;
                self.imageView.frame = NSMakeRect(0, 0, fullW, fullH);
                self.imageView.image = thumb;   // 축소 썸네일을 원본 크기 프레임에 채워 그림
                [self fitImageToWindowForPath:path];
                [self updateWindowTitle];
                hadPreview = YES;
            }
        }
    }

    self.currentImageHasPreview = hadPreview;

    // 방향키가 눌린 동안에는 각 중간 이미지의 무거운 원본 디코딩을 큐에 넣지 않는다.
    // 키를 놓을 때 마지막 이미지 한 장의 원본 디코딩만 직접 요청한다.
    if (previewOnly) {
        return;
    }

    // 2) 전체 해상도는 백그라운드에서 디코딩
    [self loadFullResolutionAtPath:path hadPreview:hadPreview];
}

// === 같은 디렉토리 내 이미지 탐색 ===

// path 가 속한 디렉토리의 이미지 파일들을 정렬해 목록으로 만들고 현재 인덱스를 찾는다.
- (void)updateDirectoryListForPath:(NSString*)path {
    // 확장자 집합은 한 번만 만들어 재사용한다.
    static NSSet<NSString*>* exts = nil;
    if (!exts) {
        exts = [[NSSet alloc] initWithArray:@[ @"jpg", @"jpeg", @"png", @"webp", @"gif",
                                               @"tif", @"tiff", @"avif", @"heic", @"heif",
                                               @"qoi", @"jxl", @"bmp", @"tga", @"pbm",
                                               @"pgm", @"pnm", @"ppm" ]];
    }

    NSString* absPath = [NSURL fileURLWithPath:path].path;   // 절대 경로로 정규화
    NSString* dir = [absPath stringByDeletingLastPathComponent];

    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray<NSString*>* entries = [fm contentsOfDirectoryAtPath:dir error:nil];

    NSMutableArray<NSString*>* images = [NSMutableArray array];
    for (NSString* name in entries) {
        if ([exts containsObject:name.pathExtension.lowercaseString]) {
            [images addObject:[dir stringByAppendingPathComponent:name]];
        }
    }
    // Finder 처럼 자연스러운 순서로 정렬(파일명 기준)
    [images sortUsingComparator:^NSComparisonResult(NSString* a, NSString* b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];

    self.directoryImages = images;
    self.currentIndex = [images indexOfObject:absPath];
    if (self.currentIndex == NSNotFound) {
        self.currentIndex = 0;
    }
}

// === 썸네일 프리로드(백그라운드) ===

// 썸네일을 캐시에 넣는다(메인 스레드 전용). 한도를 넘으면 현재 위치 주변 창(window)
// 밖의 항목을 제거해 메모리를 제한한다.
- (void)storeThumbnail:(NSImage*)thumb size:(NSSize)fullSize forPath:(NSString*)path {
    if (!thumb || fullSize.width < 1 || fullSize.height < 1 || path.length == 0) {
        return;
    }
    self.thumbCache[path] = thumb;
    self.thumbSizeCache[path] = [NSValue valueWithSize:fullSize];

    if (self.thumbCache.count <= kThumbCacheLimit) {
        return;
    }
    // 현재 인덱스 ±kThumbPreloadRadius 범위(순환)만 남기고 제거
    NSArray<NSString*>* list = self.directoryImages;
    const NSInteger n = (NSInteger)list.count;
    NSMutableSet<NSString*>* keep = [NSMutableSet set];
    if (n > 0) {
        for (NSInteger d = -kThumbPreloadRadius; d <= kThumbPreloadRadius; d++) {
            [keep addObject:list[((self.currentIndex + d) % n + n) % n]];
        }
    }
    for (NSString* key in self.thumbCache.allKeys) {
        if (![keep containsObject:key]) {
            [self.thumbCache removeObjectForKey:key];
            [self.thumbSizeCache removeObjectForKey:key];
        }
    }
}

// 현재 이미지 기준 앞뒤 kThumbPreloadRadius 장의 썸네일을 백그라운드에서 미리 만든다.
// 경로마다 독립 작업으로 등록하므로 CPU 코어 수만큼 병렬 실행된다. 방향키 탐색으로
// 중심이 바뀌어도 이미 시작한 작업은 취소하지 않아, 탐색 중에도 캐시가 계속 쌓인다.
- (void)preloadThumbnailsAroundCurrentIndex {
    NSArray<NSString*>* list = self.directoryImages;
    const NSInteger n = (NSInteger)list.count;
    if (n <= 1) {
        return;
    }
    const NSInteger center = self.currentIndex;

    // 가까운 순서(다음 → 이전 → 그다음 …)로, 아직 캐시에 없는 경로만 모은다.
    NSMutableOrderedSet<NSString*>* pending = [NSMutableOrderedSet orderedSet];
    for (NSInteger d = 1; d <= MIN(kThumbPreloadRadius, n - 1); d++) {
        [pending addObject:list[(center + d) % n]];              // 앞(다음) 방향 우선
        [pending addObject:list[((center - d) % n + n) % n]];    // 뒤(이전) 방향
    }
    for (NSString* path in pending) {
        // 이 메서드와 완료 블록은 모두 메인 스레드에서 thumbCache/thumbInFlight 를
        // 만진다. 빠른 방향키 반복으로 같은 파일이 여러 번 큐에 들어가는 것을 막는다.
        if (self.thumbCache[path] || [self.thumbInFlight containsObject:path]) {
            continue;
        }
        [self.thumbInFlight addObject:path];

        [self.thumbQueue addOperationWithBlock:^{
            @autoreleasepool {
                NSSize fullSize = NSZeroSize;
                NSImage* thumb = FastThumbnail(path, kPreviewMaxPixel, &fullSize);
                if (!thumb) {
                    // ImageIO 미지원 포맷(QOI, 구형 macOS 의 JXL 등) → 자체 디코더로 축소
                    thumb = DecoderThumbnail(path, kPreviewMaxPixel, &fullSize);
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self.thumbInFlight removeObject:path];
                    // 경로 기준 캐시라 라운드가 바뀌어도 결과는 유효 → 항상 저장
                    if (thumb) {
                        [self storeThumbnail:thumb size:fullSize forPath:path];
                    }
                });
            }
        }];
    }
}

// 다음/이전 이미지로 넘긴다(끝에서 순환). 창 크기는 유지.
- (void)showNextImage:(id)sender {
    NSInteger n = (NSInteger)self.directoryImages.count;
    if (n <= 1) {
        return;
    }
    self.currentIndex = (self.currentIndex + 1) % n;
    [self showImageAtPath:self.directoryImages[self.currentIndex]];
}

- (void)showPreviousImage:(id)sender {
    NSInteger n = (NSInteger)self.directoryImages.count;
    if (n <= 1) {
        return;
    }
    self.currentIndex = (self.currentIndex - 1 + n) % n;
    [self showImageAtPath:self.directoryImages[self.currentIndex]];
}

// 방향키(←/→, 수식키 없이)로 이미지를 넘기도록 로컬 이벤트 모니터를 설치한다.
// 시스템 키 반복 대신 자체 타이머로 반복해, 꾹 누르고 있을 때 훨씬 빠르게 넘어간다.
// (앱 생명주기 동안 존재하는 델리게이트이므로 self 를 직접 캡처)

static const NSTimeInterval kArrowRepeatInitialDelay = 0.20;  // 첫 반복까지 대기(초)
static const NSTimeInterval kArrowRepeatInterval     = 0.05;  // 이후 반복 간격(초)
static const NSTimeInterval kArrowThumbnailModeDelay = 1.00;  // 이 시간 이상 누르면 썸네일만 표시

- (void)installArrowKeyNavigation {
    [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp)
                                          handler:^NSEvent*(NSEvent* e) {
        BOOL isArrow = (e.keyCode == 123 || e.keyCode == 124);   // ← / →

        if (e.type == NSEventTypeKeyUp) {
            if (isArrow && e.keyCode == self.heldArrowKeyCode) {
                [self stopArrowKeyRepeat];
                return nil;
            }
            return e;
        }

        if (!self.window.isKeyWindow || self.directoryImages.count <= 1) {
            return e;
        }
        // Command/Option/Control 이 눌린 경우는 그대로 통과(메뉴 단축키 등)
        NSEventModifierFlags mods = e.modifierFlags &
            (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl);
        if (mods != 0) {
            return e;
        }
        if (!isArrow) {
            return e;
        }
        // 시스템 키 반복 이벤트는 무시(자체 타이머가 대신 반복)
        if (e.isARepeat) {
            return nil;
        }
        [self startArrowKeyRepeatForKeyCode:e.keyCode];
        [self navigateForArrowKeyCode:e.keyCode];
        return nil;                      // 이벤트 소비(스크롤 방지)
    }];
}

- (void)navigateForArrowKeyCode:(unsigned short)keyCode {
    if (keyCode == 123) {
        [self showPreviousImage:nil];
    } else {
        [self showNextImage:nil];
    }
}

- (void)startArrowKeyRepeatForKeyCode:(unsigned short)keyCode {
    // 새 방향키 입력은 다시 원본 로드 모드에서 시작한다.
    [self.keyRepeatTimer invalidate];
    [self.thumbnailModeTimer invalidate];
    self.keyRepeatTimer = nil;
    self.thumbnailModeTimer = nil;
    self.heldArrowKeyCode = keyCode;
    self.arrowThumbnailOnly = NO;

    NSTimer* t = [NSTimer timerWithTimeInterval:kArrowRepeatInterval
                                         target:self
                                       selector:@selector(tickArrowKeyRepeat:)
                                       userInfo:nil
                                        repeats:YES];
    t.fireDate = [NSDate dateWithTimeIntervalSinceNow:kArrowRepeatInitialDelay];
    // 이벤트 트래킹 중에도 동작하도록 common 모드에 등록
    [[NSRunLoop currentRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    self.keyRepeatTimer = t;

    NSTimer* thumbnailTimer = [NSTimer timerWithTimeInterval:kArrowThumbnailModeDelay
                                                      target:self
                                                    selector:@selector(enterArrowThumbnailMode:)
                                                    userInfo:nil
                                                     repeats:NO];
    [[NSRunLoop currentRunLoop] addTimer:thumbnailTimer forMode:NSRunLoopCommonModes];
    self.thumbnailModeTimer = thumbnailTimer;
}

- (void)stopArrowKeyRepeat {
    const BOOL wasNavigating = self.heldArrowKeyCode != 0;
    const BOOL wasThumbnailOnly = self.arrowThumbnailOnly;
    [self.keyRepeatTimer invalidate];
    [self.thumbnailModeTimer invalidate];
    self.keyRepeatTimer = nil;
    self.thumbnailModeTimer = nil;
    self.heldArrowKeyCode = 0;
    self.arrowThumbnailOnly = NO;

    // 1초 이상 눌러 썸네일 모드에 들어갔던 경우에만, 키를 놓은 시점의 마지막
    // 이미지 한 장을 원본 해상도로 디코딩한다. 짧은 입력은 이미 원본을 요청했다.
    if (wasNavigating && wasThumbnailOnly && self.currentIndex >= 0 &&
        self.currentIndex < (NSInteger)self.directoryImages.count) {
        NSString* path = self.directoryImages[self.currentIndex];
        [self loadFullResolutionAtPath:path hadPreview:self.currentImageHasPreview];
    }
}

- (void)enterArrowThumbnailMode:(NSTimer*)timer {
    self.thumbnailModeTimer = nil;
    if (self.heldArrowKeyCode == 0 || !self.window.isKeyWindow) {
        return;
    }

    self.arrowThumbnailOnly = YES;

    // 현재 진행 중인 중간 이미지 원본 결과를 무효화하고, 1초가 지난 시점의
    // 이미지도 즉시 1/8 미리보기/캐시 이미지로 교체한다.
    if (self.currentIndex >= 0 &&
        self.currentIndex < (NSInteger)self.directoryImages.count) {
        NSString* path = self.directoryImages[self.currentIndex];
        [self showImageAtPath:path];
    }
}

- (void)tickArrowKeyRepeat:(NSTimer*)timer {
    if (!self.window.isKeyWindow || self.directoryImages.count <= 1) {
        [self stopArrowKeyRepeat];
        return;
    }
    [self navigateForArrowKeyCode:self.heldArrowKeyCode];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
}

@end

// ---------------------------------------------------------------------------
// C++ 진입점(main.cpp)에서 호출되는 Cocoa 런처
// ---------------------------------------------------------------------------
int run_viewer(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        ApplyBundleApplicationIcon(app);

        AppDelegate* delegate = [[AppDelegate alloc] init];
        if (argc > 1) {
            delegate.startupPath = [NSString stringWithUTF8String:argv[1]];
        }
        app.delegate = delegate;

        [app run];
    }
    return 0;
}
