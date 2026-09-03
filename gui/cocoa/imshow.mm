#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include <decoder/image_decoder.h>

#include "imshow.h"

static NSImage* make_ns_image(const std::vector<std::uint8_t>& pixels,
                              const int width,
                              const int height) {
    if (width <= 0 || height <= 0) {
        return nil;
    }
    const std::size_t byte_count =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (pixels.size() < byte_count) {
        return nil;
    }

    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:nullptr
                          pixelsWide:width
                          pixelsHigh:height
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:width * 4
                        bitsPerPixel:32];
    if (bitmap == nil) {
        return nil;
    }

    std::memcpy(bitmap.bitmapData, pixels.data(), byte_count);

    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:bitmap];
    return image;
}

@interface FIVImageWindow : NSWindow
@end

@implementation FIVImageWindow

- (void)keyDown:(NSEvent*)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@" "]) {
        [self performClose:nil];
        return;
    }

    [super keyDown:event];
}

@end

@interface FIVImageWindowController : NSObject <NSWindowDelegate> {
    NSWindow* _window;
    NSImageView* _imageView;
    NSArray<NSImage*>* _images;
    NSArray<NSNumber*>* _frameDelays;
    NSUInteger _frameIndex;
    NSTimer* _animationTimer;
}

- (instancetype)initWithDecodedImage:(const DecodedImage&)decoded
                                title:(NSString*)title;
- (void)showWindow;

@end

@implementation FIVImageWindowController

- (instancetype)initWithDecodedImage:(const DecodedImage&)decoded
                                title:(NSString*)title {
    self = [super init];
    if (self == nil) {
        return nil;
    }

    NSMutableArray<NSImage*>* images = [NSMutableArray array];
    NSMutableArray<NSNumber*>* delays = [NSMutableArray array];

    if (decoded.animated()) {
        for (const ImageFrame& frame : decoded.frames) {
            NSImage* image = make_ns_image(frame.pixels, decoded.width, decoded.height);
            if (image != nil) {
                [images addObject:image];
                [delays addObject:@(std::max(frame.delay_ms, 10))];
            }
        }
    } else {
        NSImage* image = make_ns_image(decoded.pixels, decoded.width, decoded.height);
        if (image != nil) {
            [images addObject:image];
        }
    }

    if (images.count == 0) {
        return nil;
    }

    _images = [images copy];
    _frameDelays = [delays copy];
    _frameIndex = 0;

    const NSRect contentRect = NSMakeRect(0, 0, decoded.width, decoded.height);
    const NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                    NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskMiniaturizable |
                                    NSWindowStyleMaskResizable;
    _window = [[FIVImageWindow alloc] initWithContentRect:contentRect
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    _window.title = title.length > 0 ? title : @"FreeImageViewer";
    _window.delegate = self;
    _window.releasedWhenClosed = NO;

    _imageView = [[NSImageView alloc] initWithFrame:contentRect];
    _imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
    _imageView.imageAlignment = NSImageAlignCenter;
    _imageView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _imageView.image = _images.firstObject;
    _window.contentView = _imageView;

    return self;
}

- (void)scheduleNextFrame {
    if (_images.count < 2 || _frameDelays.count != _images.count) {
        return;
    }

    const NSTimeInterval delay = _frameDelays[_frameIndex].doubleValue / 1000.0;
    _animationTimer = [NSTimer scheduledTimerWithTimeInterval:delay
                                                       target:self
                                                     selector:@selector(showNextFrame:)
                                                     userInfo:nil
                                                      repeats:NO];
}

- (void)showNextFrame:(NSTimer*)timer {
    (void)timer;
    _frameIndex = (_frameIndex + 1) % _images.count;
    _imageView.image = _images[_frameIndex];
    [self scheduleNextFrame];
}

- (void)showWindow {
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [self scheduleNextFrame];
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [_animationTimer invalidate];
    _animationTimer = nil;
    [NSApp stop:nil];
}

@end

void imshow(const char* image_path) {
    if (image_path == nullptr || image_path[0] == '\0') {
        throw std::invalid_argument("imshow: image_path is empty");
    }
    if (![NSThread isMainThread]) {
        throw std::runtime_error("imshow must be called from the main thread");
    }

    const DecodedImage decoded = decode_image(std::string(image_path), true);
    if (!decoded.ok) {
        throw std::runtime_error("imshow: " + decoded.error);
    }

    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application finishLaunching];

        NSString* path = [NSString stringWithUTF8String:image_path];
        FIVImageWindowController* controller = [[FIVImageWindowController alloc]
                initWithDecodedImage:decoded
                               title:path.lastPathComponent];
        if (controller == nil) {
            throw std::runtime_error("imshow: failed to create an NSImage");
        }

        [controller showWindow];
        [application activateIgnoringOtherApps:YES];
        [application run];
    }
}
