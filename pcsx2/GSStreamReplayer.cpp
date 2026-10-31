// SPDX-FileCopyrightText: 2026 GT4Recomp contributors (based on GSDumpReplayer, 2002-2026 PCSX2 Dev Team)
// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp fork: executes GS stream protocol 1.x records (see GSStreamReplayer.h) on the PCSX2 GS.
//
// Per record:
//   GIF        the GIF bytes go to the GIF unit path named in the record (PATH1/2/3), as completed GS packets, like the
//              dump replayer's transfer packets; parts flagged MORE are joined first.
//   PRIV       stored in the 8 KiB privileged block (PS2MEM_GS). CSR: bit 9 (RESET) resets the privileged block, IMR,
//              CSR and the GS, like a CSR reset on the EE side; bits 0-4 clear interrupt flags; the value itself is not
//              stored. IMR keeps the mask bits only. PMODE/DISPFB1/DISPFB2 writes mark "registers written" for the
//              next VSync, as the EE GS write handler does.
//   VSYNC      CSR FIELD is set to the record's field, the display field of the frame that ends here (protocol 1.1;
//              1.0 producers sent the same values), then the vsync is posted to the GS thread, which presents with
//              it (GSvsync field = !CSR.FIELD, as for PCSX2's EE). Records without a valid field alternate it.
//              vsyncs_done is bumped on the GS thread after the vsync was processed. Until 0.3.1 the field of the
//              PREVIOUS record was used, which wove 480i field-mode frames in the wrong order once SYNCV marked the
//              output as interlaced (task 047, docs/GS-LIVE.md section 8 of the game repository).
//   READ_FIFO  local-to-host read through MTGS::InitAndReadFIFO into the response area, then readback_done. The GS
//              hardware download mode ([EmuCore/GS] HWDownloadMode) decides how: Enabled (accurate, the default) waits for
//              the GS thread and the GPU; Unsynchronized answers on this thread from the GS local memory as it is (no GS
//              thread or GPU wait, may be stale); Disabled answers zeros. Unsynchronized needs the transfer registers
//              (BITBLTBUF, TRXPOS, TRXREG), which the EE side of PCSX2 captures in Gif_HandlerAD; here a small GIF tag
//              scanner per path captures them from the A+D writes of the stream (task 037).
//   RESET      kind 1: GS hard reset (privileged block cleared); kind 2: GIF path parsers soft reset.
//   CONTROL    shutdown, attach_window, resize, snapshot; reload_config is answered "unsupported" (TASK-039); cursor
//              (protocol 1.2, gt4-gs 1.0.0) hands the game's cursor auto-hide policy to the window thread (task 060);
//              display_aspect (protocol 1.3, gt4-gs 1.1.0, kind 7) switches the presented picture between the configured
//              aspect (mode 0) and fill (mode 1, PCSX2's Stretch), queued on the GS thread so it applies from the next
//              presented frame; any other mode is answered ERROR bad_argument (not fatal) and changes nothing.
//   OSD_STATS  (protocol 1.2) the game's performance numbers for the overlay (GSStreamOverlay, task 062). READY asks
//              for them (kReadyFlagOsdStats) only while the overlay is on. They never change the picture.
//
// The session starts with a clean GS (hard reset), never with a freeze state: a GS dump without a state makes the
// dump replayer report "Failed to load GS state" and keep the state the GS had at open time; the stream always starts
// at the game's power-on, so an explicit hard reset gives the same result without the error.

#include "GSStreamReplayer.h"

#include "GS.h"
#include "GS/GS.h"
#include "GSDumpReplayer.h"
#include "GSStreamOverlay.h"
#include "Gif_Unit.h"
#include "Host.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "Vif_Dma.h"

#include "common/Console.h"
#include "common/RedtapeWindows.h"
#include "common/Timer.h"

#include "gs_stream_protocol.h"

#include "fmt/format.h"

#include <algorithm>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <cstring>
#include <memory>
#include <vector>

namespace proto = gs_stream;

static void StreamCpuReserve() {}
static void StreamCpuShutdown() {}
static void StreamCpuReset() {}
static void StreamCpuStep() {}
static void StreamCpuExecute();
static void StreamCpuExitExecution();
static void StreamCpuCancelInstruction() {}
static void StreamCpuClear(u32 addr, u32 size) {}

R5900cpu GSStreamReplayerCpu = {
	StreamCpuReserve,
	StreamCpuShutdown,
	StreamCpuReset,
	StreamCpuStep,
	StreamCpuExecute,
	StreamCpuExitExecution,
	StreamCpuCancelInstruction,
	StreamCpuClear};

namespace
{
	// Privileged register offsets inside the 8 KiB block.
	constexpr u32 kPmode = 0x0000;
	constexpr u32 kDispfb1 = 0x0070;
	constexpr u32 kDispfb2 = 0x0090;
	constexpr u32 kCsr = 0x1000;
	constexpr u32 kImr = 0x1010;
	constexpr u32 kCsrFieldBit = 0x2000;
	constexpr u32 kStatsEveryVsyncs = 500;
	constexpr u32 kTransferRegisterFirst = 0x50; // BITBLTBUF
	constexpr u32 kTransferRegisterLast = 0x52; // TRXREG

	std::unique_ptr<GSStreamReplayer::Source> s_source;
	GSStreamReplayer::WindowHooks s_hooks;
	std::string s_renderer_name;
	std::string s_version;

	bool s_started = false;
	bool s_finished = false;
	bool s_running = false;
	int s_exit_code = 1;

	u64 s_display_aspect_mode = GSStreamReplayer::Protocol13::kDisplayAspectGame; // protocol 1.3 CONTROL display_aspect
	bool s_registers_written = false;
	bool s_display_field = false; // CSR FIELD of the last VSYNC (used to alternate when a record has no valid field)
	std::vector<u8> s_gif_parts; // GIF parts flagged MORE, joined until the last part
	u8 s_gif_parts_path = 0;
	std::vector<u8> s_readback_buffer;
	std::vector<u32> s_snapshot_pixels;

	// GIF tag scanner per path: follows PACKED/REGLIST/IMAGE tags across records and copies A+D writes to BITBLTBUF,
	// TRXPOS and TRXREG (0x50-0x52) into vif1.transfer_registers, as PCSX2's Gif_HandlerAD does on the EE side. Only the
	// unsynchronized readback mode reads these registers.
	struct GifTransferRegisterScanner
	{
		u32 loops_left = 0; // NLOOP still to go for the current tag (0: the next qword is a tag)
		u32 registers = 0; // NREG (1-16)
		u32 register_index = 0; // next register descriptor in PACKED mode
		u64 descriptors = 0; // REGS
		u32 format = 0; // FLG: 0 PACKED, 1 REGLIST, 2/3 IMAGE
		u64 words_left = 0; // REGLIST: 64-bit words still to go (padded to a qword at the end)

		void Scan(const u8* data, u32 bytes)
		{
			for (u32 at = 0; at + 16 <= bytes; at += 16)
			{
				const u8* qword = data + at;
				u64 low, high;
				std::memcpy(&low, qword, 8);
				std::memcpy(&high, qword + 8, 8);
				if (loops_left == 0)
				{
					loops_left = static_cast<u32>(low & 0x7FFF);
					format = static_cast<u32>((low >> 58) & 3);
					registers = static_cast<u32>((low >> 60) & 0xF);
					if (registers == 0)
						registers = 16;
					descriptors = high;
					register_index = 0;
					words_left = static_cast<u64>(loops_left) * registers;
					continue;
				}
				if (format == 0)
				{
					const u32 descriptor = static_cast<u32>((descriptors >> (4 * register_index)) & 0xF);
					if (descriptor == 0xE) // A+D: data in bits 0-63, register address in bits 64-71
					{
						const u32 address = static_cast<u32>(high & 0xFF);
						if (address >= kTransferRegisterFirst && address <= kTransferRegisterLast)
							vif1.transfer_registers[address - kTransferRegisterFirst] = low;
					}
					if (++register_index == registers)
					{
						register_index = 0;
						--loops_left;
					}
				}
				else if (format == 1)
				{
					// Two 64-bit words per qword; an odd total leaves the upper half of the last qword unused.
					words_left = words_left > 2 ? words_left - 2 : 0;
					if (words_left == 0)
						loops_left = 0;
				}
				else
				{
					--loops_left; // IMAGE: NLOOP qwords of data
				}
			}
		}
	};
	GifTransferRegisterScanner s_transfer_scanners[3];
	// GIF records that started while another path was inside a GIF packet. The PCSX2 GS parses the packets of all paths
	// with one tag state, so the stream is expected to deliver whole packets per record; non-zero means it did not.
	u64 s_gif_interleaved = 0;

	// Readback service times on the CPU thread (READ_FIFO taken from the ring until the reply is written), per
	// periodic report and for the whole session.
	struct ReadbackTimes
	{
		static constexpr int kBuckets = 8; // <0.25, <0.5, <1, <2, <4, <8, <16, >=16 ms
		u64 count = 0;
		u64 total_us = 0;
		u64 max_us = 0;
		u64 buckets[kBuckets] = {};

		void Add(u64 us)
		{
			++count;
			total_us += us;
			max_us = std::max(max_us, us);
			int bucket = 0;
			for (u64 limit = 250; bucket < kBuckets - 1 && us >= limit; limit *= 2)
				++bucket;
			++buckets[bucket];
		}

		std::string Text() const
		{
			return fmt::format("{} served, mean {:.3f} ms, max {:.3f} ms, buckets <0.25:{} <0.5:{} <1:{} <2:{} <4:{} <8:{} "
							   "<16:{} >=16:{}",
				count, count ? total_us / 1000.0 / count : 0.0, max_us / 1000.0, buckets[0], buckets[1], buckets[2],
				buckets[3], buckets[4], buckets[5], buckets[6], buckets[7]);
		}
	};
	// GS thread frame times: the time between two consecutive vsyncs processed on the GS thread. In a file replay
	// (unthrottled) this is the GS work of one frame, so shader and pipeline compiles show up as spikes; live it also
	// contains the wait for the game's next frame. Written on the GS thread, read after MTGS::WaitGS.
	struct GsFrameTimes
	{
		Common::Timer::Value last = 0;
		u64 frames = 0, over_17ms = 0, over_33ms = 0, over_50ms = 0, over_100ms = 0;
		double max_ms = 0.0, total_ms = 0.0;

		void Vsync()
		{
			const Common::Timer::Value now = Common::Timer::GetCurrentValue();
			if (last != 0)
			{
				const double ms = Common::Timer::ConvertValueToMilliseconds(now - last);
				++frames;
				total_ms += ms;
				max_ms = std::max(max_ms, ms);
				over_17ms += ms > 16.7;
				over_33ms += ms > 33.3;
				over_50ms += ms > 50.0;
				over_100ms += ms > 100.0;
			}
			last = now;
		}
	};
	GsFrameTimes s_gs_frames;

	ReadbackTimes s_readback_interval;
	ReadbackTimes s_readback_total;
	u64 s_report_acquire_us = 0, s_report_acquire_count = 0, s_report_acquire_blocked = 0;

	// Counters (CPU thread).
	u64 s_frame = 0;
	u64 s_records = 0;
	u64 s_bytes = 0;
	u64 s_readbacks = 0;
	u64 s_busy_us = 0;
	u64 s_wait_us = 0;
	u64 s_gif_bytes = 0;
	// Values at the last periodic report.
	u64 s_report_frame = 0, s_report_busy_us = 0, s_report_wait_us = 0, s_report_bytes = 0;

	u64 RecordBytes(u32 payload_bytes) { return proto::record_size(payload_bytes); }

	void PostError(proto::ErrorCode code, bool fatal, const std::string& text)
	{
		Console.Error(fmt::format("(GSStreamReplayer) {}{}", text, fatal ? " (fatal)" : ""));
		proto::ErrorPayload payload{};
		payload.code = static_cast<u32>(code);
		payload.fatal = fatal ? 1 : 0;
		std::strncpy(payload.text, text.c_str(), sizeof(payload.text) - 1);
		if (s_source)
			s_source->PostEvent(static_cast<u16>(proto::RecordType::error), &payload, sizeof(payload));
	}

	const char* ReadbackModeName()
	{
		switch (EmuConfig.GS.HWDownloadMode)
		{
			case GSHardwareDownloadMode::Enabled: return "accurate";
			case GSHardwareDownloadMode::EnabledForceFull: return "accurate, full";
			case GSHardwareDownloadMode::NoReadbacks: return "no GPU readbacks";
			case GSHardwareDownloadMode::Unsynchronized: return "unsynchronized";
			case GSHardwareDownloadMode::Disabled: return "disabled";
		}
		return "unknown";
	}

	void PostStats()
	{
		proto::StatsPayload stats{};
		stats.vsyncs = s_frame;
		stats.records = s_records;
		stats.bytes = s_bytes;
		stats.readbacks = s_readbacks;
		stats.readbacks_zero_filled = 0;
		stats.busy_us = s_busy_us;
		stats.wait_us = s_wait_us;
		s_source->PostEvent(static_cast<u16>(proto::RecordType::stats), &stats, sizeof(stats));

		// Same interval as a console line, with the GS thread and GPU times PCSX2 measures itself.
		const double frames = static_cast<double>(std::max<u64>(1, s_frame - s_report_frame));
		Console.WriteLn(fmt::format("(GSStreamReplayer) vsync {}: stream {:.0f} bytes/frame; CPU thread busy {:.3f} "
									"ms/frame, waiting {:.3f} ms/frame; GS thread {:.3f} ms/frame ({:.1f}%); GPU {:.3f} ms "
									"({:.1f}%)",
			s_frame, (s_bytes - s_report_bytes) / frames, (s_busy_us - s_report_busy_us) / 1000.0 / frames,
			(s_wait_us - s_report_wait_us) / 1000.0 / frames, PerformanceMetrics::GetGSThreadAverageTime(),
			PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetGPUAverageTime(),
			PerformanceMetrics::GetGPUUsage()));
		// Task 037: readback service times and the time the GS thread waited for a swap chain image (with FIFO
		// presentation the acquire blocks until the display releases an image at its refresh).
		const u64 acquire_us = GSGetSwapChainAcquireWaitUs();
		const u64 acquire_count = GSGetSwapChainAcquireCount();
		const u64 acquire_blocked = GSGetSwapChainAcquireBlockedCount();
		Console.WriteLn(fmt::format("(GSStreamReplayer) vsync {}: readbacks ({}) {}; swap chain acquire {:.3f} ms/frame, "
									"{} acquires, {} blocked over 1 ms",
			s_frame, ReadbackModeName(), s_readback_interval.Text(), (acquire_us - s_report_acquire_us) / 1000.0 / frames,
			acquire_count - s_report_acquire_count, acquire_blocked - s_report_acquire_blocked));
		s_readback_interval = {};
		s_report_acquire_us = acquire_us;
		s_report_acquire_count = acquire_count;
		s_report_acquire_blocked = acquire_blocked;
		s_report_frame = s_frame;
		s_report_busy_us = s_busy_us;
		s_report_wait_us = s_wait_us;
		s_report_bytes = s_bytes;
	}

	void Finish(int exit_code, bool post_exiting)
	{
		if (s_finished)
			return;
		s_finished = true;
		s_running = false;
		s_exit_code = exit_code;
		if (MTGS::IsOpen())
			MTGS::WaitGS(false, false, false);
		if (s_source && s_source->IsLive() && post_exiting)
		{
			PostStats();
			s_source->SetStateBits(proto::kStateExiting);
			proto::ExitingPayload exiting{};
			exiting.exit_code = static_cast<u32>(exit_code);
			s_source->PostEvent(static_cast<u16>(proto::RecordType::exiting), &exiting, sizeof(exiting));
		}
		Console.WriteLn(fmt::format("(GSStreamReplayer) finished: {} vsyncs, {} records, {} bytes ({} GIF bytes), {} "
									"readbacks; exit code {}",
			s_frame, s_records, s_bytes, s_gif_bytes, s_readbacks, exit_code));
		if (s_readback_total.count)
			Console.WriteLn(fmt::format("(GSStreamReplayer) readbacks ({}): {}", ReadbackModeName(), s_readback_total.Text()));
		Console.WriteLn(fmt::format("(GSStreamReplayer) GIF records started inside another path's packet: {}", s_gif_interleaved));
		const GsFrameTimes& g = s_gs_frames;
		Console.WriteLn(fmt::format("(GSStreamReplayer) GS thread frame times: {} frames, mean {:.3f} ms, max {:.1f} ms; over "
									"16.7 ms {}, over 33.3 ms {}, over 50 ms {}, over 100 ms {}",
			g.frames, g.frames ? g.total_ms / g.frames : 0.0, g.max_ms, g.over_17ms, g.over_33ms, g.over_50ms, g.over_100ms));
		Host::RequestVMShutdown(false, false, false);
	}

	void HardReset()
	{
		if (MTGS::IsOpen())
			MTGS::WaitGS(false, false, false);
		std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		CSRreg.Reset();
		GSIMR.reset();
		s_display_field = false;
		s_registers_written = false;
		s_gif_parts.clear();
		for (GifTransferRegisterScanner& scanner : s_transfer_scanners)
			scanner = {};
		std::memset(vif1.transfer_registers, 0, sizeof(vif1.transfer_registers));
		MTGS::ResetGS(true);
	}

	void SendGif(u8 path, const u8* data, u32 bytes)
	{
		if (bytes == 0)
			return;
		const GIF_PATH gif_path = static_cast<GIF_PATH>(path - 1);
		for (u32 other = 0; other < 3; ++other)
		{
			if (other != static_cast<u32>(path - 1) && s_transfer_scanners[other].loops_left != 0)
			{
				++s_gif_interleaved;
				break;
			}
		}
		s_transfer_scanners[path - 1].Scan(data, bytes);
		Gif_Path& unit_path = gifUnit.gifPath[gif_path];
		unit_path.CopyGSPacketData(const_cast<u8*>(data), bytes);
		GS_Packet packet;
		packet.offset = unit_path.curOffset;
		packet.size = bytes;
		unit_path.curOffset += bytes;
		Gif_AddCompletedGSPacket(packet, gif_path);
		s_gif_bytes += bytes;
	}

	bool DoGif(const GSStreamReplayer::Record& record)
	{
		proto::GifPayload gif;
		if (record.payload_bytes < sizeof(gif))
			return false;
		std::memcpy(&gif, record.payload, sizeof(gif));
		if (gif.path < 1 || gif.path > 3 || (gif.bytes % 16) != 0 || sizeof(gif) + gif.bytes > record.payload_bytes)
			return false;
		const u8* data = record.payload + sizeof(gif);
		if ((record.flags & proto::kRecordFlagMore) || !s_gif_parts.empty())
		{
			if (!s_gif_parts.empty() && s_gif_parts_path != gif.path)
				return false;
			s_gif_parts_path = gif.path;
			s_gif_parts.insert(s_gif_parts.end(), data, data + gif.bytes);
			if (record.flags & proto::kRecordFlagMore)
				return true;
			SendGif(gif.path, s_gif_parts.data(), static_cast<u32>(s_gif_parts.size()));
			s_gif_parts.clear();
			return true;
		}
		SendGif(gif.path, data, gif.bytes);
		return true;
	}

	bool DoPriv(const GSStreamReplayer::Record& record)
	{
		proto::PrivPayload priv;
		if (record.payload_bytes < sizeof(priv))
			return false;
		std::memcpy(&priv, record.payload, sizeof(priv));
		if (priv.address < proto::kPrivilegedBase || priv.address >= proto::kPrivilegedBase + proto::kPrivilegedBytes ||
			(priv.address & 7) != 0)
			return false;
		const u32 offset = priv.address - proto::kPrivilegedBase;
		if (offset == kCsr)
		{
			if (priv.value & 0x200)
			{
				// Same effect as a CSR RESET written by the EE: privileged registers, IMR and CSR return to their reset
				// values and the GS is reset (soft).
				std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
				GSIMR.reset();
				CSRreg.Reset();
				MTGS::ResetGS(false);
			}
			GSCSRr &= ~static_cast<u32>(priv.value & 0x1F);
			return true;
		}
		if (offset == kImr)
		{
			GSIMR._u32 = (static_cast<u32>(priv.value) & 0x1f00) | 0x6000;
			return true;
		}
		std::memcpy(PS2MEM_GS + offset, &priv.value, sizeof(priv.value));
		if (offset == kPmode || offset == kDispfb1 || offset == kDispfb2)
			s_registers_written = true;
		return true;
	}

	bool DoVsync(const GSStreamReplayer::Record& record)
	{
		proto::VsyncPayload vsync;
		if (record.payload_bytes < sizeof(vsync))
			return false;
		std::memcpy(&vsync, record.payload, sizeof(vsync));
		s_display_field = (vsync.flags & proto::kVsyncFlagFieldValid) ? (vsync.field != 0) : !s_display_field;
		if (s_display_field)
			GSCSRr |= kCsrFieldBit;
		else
			GSCSRr &= ~kCsrFieldBit;
		MTGS::PostVsyncStart(s_registers_written);
		MTGS::RunOnGSThread([]() { s_gs_frames.Vsync(); });
		s_registers_written = false;
		++s_frame;
		if (s_source->IsLive())
		{
			GSStreamReplayer::Source* source = s_source.get();
			MTGS::RunOnGSThread([source]() { source->CompleteVsync(); });
		}
		VMManager::Internal::VSyncOnCPUThread();
		Host::PumpMessagesOnCPUThread();
		if (s_source->IsLive() && (s_frame % kStatsEveryVsyncs) == 0)
			PostStats();
		return true;
	}

	bool DoReadFifo(const GSStreamReplayer::Record& record)
	{
		proto::ReadFifoPayload read;
		if (record.payload_bytes < sizeof(read))
			return false;
		std::memcpy(&read, record.payload, sizeof(read));
		++s_readbacks;
		const u64 bytes = static_cast<u64>(read.qwords) * 16;
		// One extra quadword: some transfers write past the requested size (as in the dump replayer).
		s_readback_buffer.resize(static_cast<size_t>(bytes) + 16);
		Common::Timer timer;
		MTGS::InitAndReadFIFO(s_readback_buffer.data(), read.qwords);
		const u64 service_us = static_cast<u64>(timer.GetTimeNanoseconds() / 1000.0);
		s_readback_interval.Add(service_us);
		s_readback_total.Add(service_us);
		// Disabled answers zeros (MTGS::InitAndReadFIFO fills the buffer); the producer counts those replies.
		const u32 status = (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Disabled && GSIsHardwareRenderer()) ?
							   proto::kReadbackZeroFilled :
							   proto::kReadbackOk;
		if (!s_source->IsLive())
			return true;
		u64 capacity = 0;
		u8* response = s_source->ResponseArea(&capacity);
		if (!response || bytes > capacity)
		{
			PostError(proto::ErrorCode::bad_record, false, fmt::format("READ_FIFO of {} qwords exceeds the response area", read.qwords));
			s_source->CompleteReadback(read.request_id, 0, proto::kReadbackFailed);
			return true;
		}
		std::memcpy(response, s_readback_buffer.data(), static_cast<size_t>(bytes));
		s_source->CompleteReadback(read.request_id, bytes, status);
		return true;
	}

	bool DoReset(const GSStreamReplayer::Record& record)
	{
		proto::ResetPayload reset;
		if (record.payload_bytes < sizeof(reset))
			return false;
		std::memcpy(&reset, record.payload, sizeof(reset));
		if (reset.kind == static_cast<u32>(proto::ResetKind::gs_hard))
		{
			HardReset();
			return true;
		}
		if (reset.kind == static_cast<u32>(proto::ResetKind::gif_paths))
		{
			// Protocol bit N = path N; GSState::SoftReset bit 0 = PATH1, bit 1 = PATH2, bit 2 = PATH3.
			const u32 mask = (reset.path_mask >> 1) & 7;
			s_gif_parts.clear();
			for (u32 path = 0; path < 3; ++path)
			{
				if (mask & (1u << path))
					s_transfer_scanners[path] = {};
			}
			MTGS::RunOnGSThread([mask]() { GSgifSoftReset(mask); });
			return true;
		}
		return false;
	}

	void DoSnapshot(u64 id, u32 width, u32 height)
	{
		u32 out_width = 0, out_height = 0;
		s_snapshot_pixels.clear();
		const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);
		if (!MTGS::SaveMemorySnapshot(width, height, aspect_correct, true, &out_width, &out_height, &s_snapshot_pixels))
		{
			PostError(proto::ErrorCode::device_failure, false, "snapshot failed (no frame to read)");
			return;
		}
		const u64 bytes = static_cast<u64>(out_width) * out_height * 4;
		u64 capacity = 0;
		u8* response = s_source->ResponseArea(&capacity);
		if (!response || bytes > capacity || s_snapshot_pixels.size() * 4 < bytes)
		{
			PostError(proto::ErrorCode::internal, false,
				fmt::format("snapshot {}x{} does not fit the response area ({} bytes)", out_width, out_height, capacity));
			return;
		}
		std::memcpy(response, s_snapshot_pixels.data(), static_cast<size_t>(bytes));
		proto::SnapshotDonePayload done{id, out_width, out_height, 0, bytes};
		s_source->PostEvent(static_cast<u16>(proto::RecordType::snapshot_done), &done, sizeof(done));
	}

	// Protocol 1.3 CONTROL display_aspect. Mode 0 = the aspect of the settings file ([EmuCore/GS] AspectRatio, 16:9 by
	// default: the user's default), mode 1 = fill the window (Stretch). The change is queued on the GS thread behind the
	// records already sent, so it applies from the next presented frame and stays until the next display_aspect.
	// EmuConfig.CurrentAspectRatio is what the presentation (CalculateDrawDstRect) reads; nothing else in gt4-gs
	// changes it (no settings reload, no FMV switch: there is no EE).
	void SetDisplayAspect(u64 mode)
	{
		s_display_aspect_mode = mode;
		MTGS::RunOnGSThread([fill = (mode == GSStreamReplayer::Protocol13::kDisplayAspectFill)]() {
			EmuConfig.CurrentAspectRatio = fill ? AspectRatioType::Stretch : GSConfig.AspectRatio;
		});
	}

	void DoDisplayAspect(const proto::ControlPayload& control)
	{
		const u64 mode = control.args[0];
		if (mode != GSStreamReplayer::Protocol13::kDisplayAspectGame && mode != GSStreamReplayer::Protocol13::kDisplayAspectFill)
		{
			PostError(static_cast<proto::ErrorCode>(GSStreamReplayer::Protocol13::kErrorCodeBadArgument), false,
				fmt::format("CONTROL display_aspect mode {} is not 0 (game aspect) or 1 (fill); the mode stays {}", mode,
					s_display_aspect_mode));
			return;
		}
		Console.WriteLn(fmt::format("(GSStreamReplayer) CONTROL display_aspect {} ({}) from frame {}", mode,
			mode == GSStreamReplayer::Protocol13::kDisplayAspectFill ? "fill" : "game aspect", s_frame));
		SetDisplayAspect(mode);
	}

	bool DoControl(const GSStreamReplayer::Record& record)
	{
		proto::ControlPayload control;
		if (record.payload_bytes < sizeof(control))
			return false;
		std::memcpy(&control, record.payload, sizeof(control));
		if (control.kind == GSStreamReplayer::Protocol13::kControlKindDisplayAspect)
		{
			DoDisplayAspect(control);
			return true;
		}
		switch (static_cast<proto::ControlKind>(control.kind))
		{
			case proto::ControlKind::shutdown:
				Console.WriteLn("(GSStreamReplayer) CONTROL shutdown");
				Finish(0, true);
				return true;

			case proto::ControlKind::attach_window:
			{
				const u32 width = static_cast<u32>(control.args[1]);
				const u32 height = static_cast<u32>(control.args[2]);
				Console.WriteLn(fmt::format("(GSStreamReplayer) CONTROL attach_window 0x{:X} {}x{}", control.args[0], width, height));
				if (!s_source->IsLive() || !s_hooks.attach || !s_hooks.attach(control.args[0], width, height))
				{
					PostError(proto::ErrorCode::device_failure, false, "cannot attach to the game window");
					return true;
				}
				MTGS::UpdateDisplayWindow();
				MTGS::ResizeDisplayWindow(width, height, 1.0f);
				proto::WindowPayload window{static_cast<u32>(proto::WindowState::restored), 0,
					s_hooks.presentation_id ? s_hooks.presentation_id() : 0};
				s_source->PostEvent(static_cast<u16>(proto::RecordType::window), &window, sizeof(window));
				return true;
			}

			case proto::ControlKind::resize:
			{
				const u32 width = static_cast<u32>(control.args[0]);
				const u32 height = static_cast<u32>(control.args[1]);
				// A zero or oversized client area (the game window minimized) keeps the current swap chain; the child
				// window is hidden with its parent and the next real size arrives on restore (task 038).
				if (width == 0 || height == 0 || width > 16384 || height > 16384)
				{
					Console.WriteLn(fmt::format("(GSStreamReplayer) CONTROL resize {}x{} ignored", width, height));
					return true;
				}
				if (s_hooks.resize)
					s_hooks.resize(width, height);
				MTGS::ResizeDisplayWindow(width, height, 1.0f);
				return true;
			}

			case proto::ControlKind::snapshot:
				DoSnapshot(control.args[0], static_cast<u32>(control.args[1]), static_cast<u32>(control.args[2]));
				return true;

			case proto::ControlKind::reload_config:
				PostError(proto::ErrorCode::unsupported_record, false, "CONTROL reload_config is not supported yet");
				return true;

			case proto::ControlKind::cursor:
			{
				const u32 delay_ms = static_cast<u32>(std::clamp<u64>(control.args[1], 1, 60000));
				Console.WriteLn(fmt::format("(GSStreamReplayer) CONTROL cursor: auto-hide {}, {} ms",
					(control.args[0] & proto::kCursorFlagAutoHide) ? "on" : "off", delay_ms));
				if (s_source->IsLive() && s_hooks.cursor)
					s_hooks.cursor(control.args[0], delay_ms);
				return true;
			}
		}
		PostError(proto::ErrorCode::unsupported_record, false, fmt::format("unknown CONTROL kind {}", control.kind));
		return true;
	}

	// Returns false on a malformed record (fatal).
	bool Process(const GSStreamReplayer::Record& record)
	{
		switch (static_cast<proto::RecordType>(record.type))
		{
			case proto::RecordType::gif: return DoGif(record);
			case proto::RecordType::priv: return DoPriv(record);
			case proto::RecordType::vsync: return DoVsync(record);
			case proto::RecordType::read_fifo: return DoReadFifo(record);
			case proto::RecordType::reset: return DoReset(record);
			case proto::RecordType::control: return DoControl(record);
			case proto::RecordType::osd_stats:
				// Protocol 1.2: a larger payload of a later minor version keeps the known fields at the start.
				if (record.payload_bytes < sizeof(proto::OsdStatsPayload))
					return false;
				GSStreamOverlay::OnOsdStats(record.payload, sizeof(proto::OsdStatsPayload));
				return true;
			case proto::RecordType::state_save:
			case proto::RecordType::state_load:
			case proto::RecordType::aux_frame_begin:
			case proto::RecordType::aux_frame_end:
				PostError(proto::ErrorCode::unsupported_record, false, fmt::format("record type {} is reserved in protocol 1.3", record.type));
				return true;
			default:
				return false;
		}
	}

	void Start()
	{
		s_started = true;
		HardReset();
		SetDisplayAspect(GSStreamReplayer::Protocol13::kDisplayAspectGame); // protocol 1.3: every session starts in mode 0
		if (!s_source->IsLive())
			return;
		proto::ReadyPayload ready{};
		ready.version_major = proto::kVersionMajor;
		ready.version_minor = GSStreamReplayer::Protocol13::kConsumerVersionMinor; // 1.3 (gt4-gs 1.1.0)
#ifdef _WIN32
		ready.consumer_pid = static_cast<u32>(GetCurrentProcessId());
#else
		ready.consumer_pid = static_cast<u32>(getpid());
#endif
		ready.flags = proto::kReadyFlagPresents;
		if (GSStreamOverlay::WantsOsdStats())
			ready.flags |= proto::kReadyFlagOsdStats; // the performance overlay is on: the game sends OSD_STATS
		std::strncpy(ready.consumer, "gt4-gs", sizeof(ready.consumer));
		std::strncpy(ready.renderer, s_renderer_name.c_str(), sizeof(ready.renderer));
		std::strncpy(ready.version, s_version.c_str(), sizeof(ready.version));
		s_source->SetStateBits(proto::kStateConsumerReady);
		s_source->PostEvent(static_cast<u16>(proto::RecordType::ready), &ready, sizeof(ready));
		Console.WriteLn(fmt::format("(GSStreamReplayer) READY posted (renderer {}, {})", s_renderer_name, s_version));
	}
} // namespace

void GSStreamReplayer::Prepare(Source* source, const WindowHooks& hooks, const char* renderer_name, const char* version)
{
	s_source.reset(source);
	s_hooks = hooks;
	s_renderer_name = renderer_name ? renderer_name : "";
	s_version = version ? version : "";
	s_started = s_finished = s_running = false;
	s_exit_code = 1;
}

bool GSStreamReplayer::IsActive()
{
	return static_cast<bool>(s_source);
}

bool GSStreamReplayer::Initialize(Error* error)
{
	if (!s_source)
	{
		Error::SetString(error, "GSStreamReplayer: no stream source prepared");
		return false;
	}
	Console.WriteLn(fmt::format("(GSStreamReplayer) {} stream, serial {}", s_source->IsLive() ? "live" : "file", s_source->Serial()));
	// The GS is the only device that runs; the other CPUs are interpreters that never execute.
	Cpu = &GSStreamReplayerCpu;
	psxCpu = &psxInt;
	CpuVU0 = &CpuIntVU0;
	CpuVU1 = &CpuIntVU1;
	return true;
}

void GSStreamReplayer::Shutdown()
{
	Console.WriteLn("(GSStreamReplayer) Shutting down.");
	Cpu = nullptr;
	psxCpu = nullptr;
	CpuVU0 = nullptr;
	CpuVU1 = nullptr;
	// The source stays alive until the frontend exits (the GS thread may still complete a vsync on it).
}

std::string GSStreamReplayer::GetSerial()
{
	return s_source ? s_source->Serial() : std::string();
}

u64 GSStreamReplayer::GetFrameNumber()
{
	return s_frame;
}

int GSStreamReplayer::GetExitCode()
{
	return s_exit_code;
}

void GSStreamReplayer::ReportFatal(u32 code, const char* text)
{
	if (!s_source)
		return;
	PostError(static_cast<proto::ErrorCode>(code), true, text ? text : "fatal error");
	if (s_source->IsLive())
	{
		s_source->SetStateBits(proto::kStateExiting);
		proto::ExitingPayload exiting{};
		exiting.exit_code = 5;
		s_source->PostEvent(static_cast<u16>(proto::RecordType::exiting), &exiting, sizeof(exiting));
	}
}

void StreamCpuExecute()
{
	if (s_finished)
		return;
	if (!s_started)
		Start();
	s_running = true;
	GSStreamReplayer::Record record;
	while (s_running)
	{
		u64 wait_us = 0;
		const GSStreamReplayer::NextResult result = s_source->Next(record, &wait_us);
		s_wait_us += wait_us;
		if (result == GSStreamReplayer::NextResult::End)
		{
			Finish(0, false);
			break;
		}
		if (result == GSStreamReplayer::NextResult::PeerExited)
		{
			Console.Warning("(GSStreamReplayer) the game process exited; stopping");
			Finish(4, false);
			break;
		}
		if (result == GSStreamReplayer::NextResult::Failed)
		{
			Console.Error(fmt::format("(GSStreamReplayer) stream error: {}", s_source->LastError()));
			PostError(proto::ErrorCode::internal, true, s_source->LastError());
			Finish(5, true);
			break;
		}

		Common::Timer timer;
		++s_records;
		s_bytes += RecordBytes(record.payload_bytes);
		const bool ok = Process(record);
		const bool shutdown = s_finished;
		if (!shutdown)
			s_source->Release();
		s_busy_us += static_cast<u64>(timer.GetTimeNanoseconds() / 1000.0);
		if (!ok)
		{
			PostError(proto::ErrorCode::bad_record, true,
				fmt::format("malformed record type {} (sequence {}, {} payload bytes)", record.type, record.sequence, record.payload_bytes));
			Finish(3, true);
			break;
		}
		if (shutdown)
			break;
		if (VMManager::Internal::IsExecutionInterrupted())
			break;
	}
	s_running = false;
}

void StreamCpuExitExecution()
{
	s_running = false;
}
