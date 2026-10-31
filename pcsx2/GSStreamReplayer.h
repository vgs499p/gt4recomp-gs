// SPDX-FileCopyrightText: 2026 GT4Recomp contributors (based on GSDumpReplayer, 2002-2026 PCSX2 Dev Team)
// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp fork (gt4gs/live): a CPU provider that feeds the GS from a GT4Recomp GS stream instead of a GS dump.
// The stream is the MIT GS stream protocol 1.2 (3rdparty/gt4recomp-gs-stream/gs_stream_protocol.h): GIF transfers
// per path, privileged register writes, VSYNC with the field bit, FIFO readbacks, resets and control messages; since
// 1.2 (gt4-gs 1.0.0) also CONTROL cursor (the game window's cursor auto-hide policy) and OSD_STATS (the game's numbers
// for the performance overlay, GSStreamOverlay.h); since 1.3 (gt4-gs 1.1.0) CONTROL display_aspect (see
// Protocol13 below). A
// Source delivers the records, either live from the game's shared-memory ring or from a .gscap version 2 file; the
// transport itself lives in the frontend (pcsx2-gt4gs), so the core library does not depend on it.
//
// While a stream is active, GSDumpReplayer::IsReplayingDump() is true, so VMManager boots without a BIOS or disc,
// selects this CPU provider and takes the serial (and with it the GameDB entry) from the stream.

#pragma once

#include "common/Error.h"
#include "common/Pcsx2Defs.h"

#include <cstdint>
#include <string>

struct R5900cpu;
extern R5900cpu GSStreamReplayerCpu;

namespace GSStreamReplayer
{
	// GS stream protocol 1.3 (gt4-gs 1.1.0). The vendored MIT header (3rdparty/gt4recomp-gs-stream) is still the 1.2
	// copy of the game repository; until the game repository publishes the 1.3 header, the 1.3 additions are defined
	// here with the exact numbers of the 1.3 text (gt4gs/README.md, "Protocol 1.3"). The consumer implements 1.3 and
	// says so in READY; a 1.2 producer never sends kind 7 and works unchanged.
	namespace Protocol13
	{
		static constexpr u16 kConsumerVersionMinor = 3; // READY version_minor of gt4-gs 1.1.0
		// CONTROL kind 7 display_aspect: args[0] = mode, args[1] = args[2] = 0 (reserved).
		static constexpr u32 kControlKindDisplayAspect = 7;
		static constexpr u64 kDisplayAspectGame = 0; // the picture at the configured aspect, pillar/letterboxed
		static constexpr u64 kDisplayAspectFill = 1; // the picture stretched over the whole window
		// ERROR code for a CONTROL argument outside its range (not fatal; the record is ignored).
		static constexpr u32 kErrorCodeBadArgument = 7;
	} // namespace Protocol13

	// One record of the producer -> consumer direction, valid until Source::Release().
	struct Record
	{
		u16 type = 0;
		u16 flags = 0;
		u32 payload_bytes = 0;
		u64 sequence = 0;
		const u8* payload = nullptr;
	};

	enum class NextResult
	{
		Record, // a record was returned
		End, // end of a file stream
		PeerExited, // the producer (game) process is gone
		Failed, // transport error; the text is in Source::LastError()
	};

	// Where the records come from. Implemented by the frontend (live shared memory or a .gscap file).
	class Source
	{
	public:
		virtual ~Source() = default;
		virtual bool IsLive() const = 0;
		virtual std::string Serial() const = 0;
		// Blocks until the next record (live) or reads it (file).
		virtual NextResult Next(Record& record, u64* wait_us) = 0;
		virtual void Release() = 0;
		virtual const char* LastError() const = 0;

		// Live only (no-ops for a file).
		virtual void CompleteVsync() {}
		virtual u8* ResponseArea(u64* capacity) { *capacity = 0; return nullptr; }
		virtual void CompleteReadback(u64 request_id, u64 bytes, u32 status) {}
		virtual void PostEvent(u16 type, const void* payload, u32 bytes) {}
		virtual void SetStateBits(u32 bits) {}
	};

	// Window requests from CONTROL records, served by the frontend (called on the CPU thread).
	struct WindowHooks
	{
		bool (*attach)(u64 parent_window, u32 width, u32 height) = nullptr;
		void (*resize)(u32 width, u32 height) = nullptr;
		// Protocol 1.2 CONTROL cursor: auto-hide flags (kCursorFlag*) and the delay without movement.
		void (*cursor)(u64 flags, u32 hide_delay_ms) = nullptr;
		// macOS: the Core Animation context id of the consumer's picture layer after a successful attach. The game hosts
		// that layer in its window (CALayerHost), since another process cannot own a child of its window. It travels in
		// WindowPayload.reserved2 of the WINDOW restored event (zero on Windows, where the child window is the picture).
		u64 (*presentation_id)() = nullptr;
	};

	// Called by the frontend before VMManager::Initialize. Takes ownership of the source.
	void Prepare(Source* source, const WindowHooks& hooks, const char* renderer_name, const char* version);
	bool IsActive();

	// Called by VMManager (through the GSDumpReplayer hooks).
	bool Initialize(Error* error);
	void Shutdown();
	std::string GetSerial();

	u64 GetFrameNumber();
	// Exit code for the frontend: 0 after a CONTROL shutdown or the end of a file, non-zero otherwise.
	int GetExitCode();

	// Posts a fatal ERROR (if the event ring is usable) and EXITING; used by the frontend when the GS device
	// cannot be created after the handshake.
	void ReportFatal(u32 code, const char* text);
} // namespace GSStreamReplayer
