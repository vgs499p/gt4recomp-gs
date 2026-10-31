// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+

// GT4Recomp fork, macOS live mode: the picture is shared with the game as a remote Core Animation layer
// (MacPresentation.mm).

#pragma once

#include "common/WindowInfo.h"

#include <cstdint>

namespace GT4GSMac
{
	// A window-less view of width x height pixels for the Metal device (live mode only).
	bool CreateLiveView(WindowInfo* wi, uint32_t width, uint32_t height);
	void DestroyLiveView();

	// Makes the device's layer the root of a remote context; the game hosts it by PresentationId().
	bool AttachToGameWindow(uint32_t width, uint32_t height);
	void Resize(uint32_t width, uint32_t height);
	uint64_t PresentationId();
} // namespace GT4GSMac
