// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+

// GT4Recomp fork, macOS live mode: how gt4-gs shows its picture in the game's window.
//
// On Windows the GS window becomes a child of the game window (SetParent). macOS has no cross-process child windows,
// so the picture is shared as a Core Animation layer instead: the Metal device draws into the CAMetalLayer of a
// window-less NSView, that layer is the root of a remote CAContext, and the game shows it with a CALayerHost that
// names the context id (the technique browsers use for their GPU process). The window server composites it; there is
// no copy and no extra frame of latency. The game keeps its window, focus and input; this process never shows a window
// and has no Dock icon.
//
// CAContext and CALayerHost are private QuartzCore classes. They are declared here and looked up at run time, so a
// system without them fails the attach cleanly (the game then reports the GS process as unable to present).

#include "MacPresentation.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>

extern "C" uint32_t CGSMainConnectionID(void);

@interface CAContext : NSObject
+ (instancetype)contextWithCGSConnection:(uint32_t)connection options:(NSDictionary*)options;
@property(readonly) uint32_t contextId;
@property(retain) CALayer* layer;
@end

namespace
{
	void LayerChanged();
}

// The Metal device replaces the view's layer whenever it rebuilds its surface (GSDeviceMTL::UpdateWindow, which runs on
// every attach and window change). The shared context must always carry the current layer, so the view reports it.
@interface GT4GSLiveView : NSView
@end

@implementation GT4GSLiveView
- (void)setLayer:(CALayer*)layer
{
	[super setLayer:layer];
	LayerChanged();
}
@end

namespace
{
	GT4GSLiveView* s_view = nil; // window-less view whose layer the Metal device creates (AttachSurfaceOnMainThread)
	CAContext* s_context = nil;
	std::atomic<uint32_t> s_context_id{0};
	float s_scale = 1.0f;
	uint32_t s_width = 0, s_height = 0; // the game's drawable size in pixels (attach and resize)

	template <typename F>
	void OnMainThread(F&& function)
	{
		if ([NSThread isMainThread])
			function();
		else
			dispatch_sync(dispatch_get_main_queue(), function);
	}

	// The root layer covers the game's view: the game sends its drawable size in pixels.
	void SetLayerSize(uint32_t width, uint32_t height)
	{
		CALayer* layer = s_view.layer;
		if (!layer)
			return;
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		layer.anchorPoint = CGPointZero;
		layer.position = CGPointZero;
		layer.bounds = CGRectMake(0, 0, width / s_scale, height / s_scale);
		layer.contentsScale = s_scale;
		[CATransaction commit];
	}

	// Main thread: the device's new layer becomes the shared root (or the context is left empty while it has none).
	void LayerChanged()
	{
		if (!s_context)
			return;
		CALayer* layer = s_view.layer;
		if (layer && s_width && s_height)
			SetLayerSize(s_width, s_height);
		s_context.layer = layer;
	}
} // namespace

bool GT4GSMac::CreateLiveView(WindowInfo* wi, uint32_t width, uint32_t height)
{
	bool ok = false;
	OnMainThread([&] {
		[NSApplication sharedApplication];
		// A background process: no Dock icon, no menu bar, never activated.
		[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
		s_scale = static_cast<float>([[NSScreen mainScreen] backingScaleFactor]);
		s_view = [[GT4GSLiveView alloc] initWithFrame:NSMakeRect(0, 0, width / s_scale, height / s_scale)];
		wi->type = WindowInfo::Type::MacOS;
		wi->window_handle = (__bridge void*)s_view;
		wi->surface_width = width;
		wi->surface_height = height;
		wi->surface_scale = s_scale;
		ok = s_view != nil;
	});
	return ok;
}

void GT4GSMac::DestroyLiveView()
{
	OnMainThread([] {
		s_context.layer = nil;
		s_context = nil;
		s_context_id.store(0);
		s_view = nil;
	});
}

bool GT4GSMac::AttachToGameWindow(uint32_t width, uint32_t height)
{
	bool ok = false;
	OnMainThread([&] {
		if (!s_view || !s_view.layer)
		{
			std::fprintf(stderr, "gt4-gs: no Metal layer to share (the GS device has no surface)\n");
			return;
		}
		if (!NSClassFromString(@"CAContext"))
		{
			std::fprintf(stderr, "gt4-gs: this macOS has no remote Core Animation contexts\n");
			return;
		}
		if (!s_context)
		{
			s_context = [CAContext contextWithCGSConnection:CGSMainConnectionID() options:@{}];
			if (!s_context)
				return;
		}
		s_width = width;
		s_height = height;
		LayerChanged();
		s_context_id.store(s_context.contextId);
		ok = s_context.contextId != 0;
	});
	return ok;
}

void GT4GSMac::Resize(uint32_t width, uint32_t height)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		s_width = width;
		s_height = height;
		SetLayerSize(width, height);
	});
}

uint64_t GT4GSMac::PresentationId()
{
	return s_context_id.load();
}
