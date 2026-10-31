// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team, 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+
//
// gt4-gs.exe: the GT4Recomp GS process (fork of pcsx2-gsrunner/Main.cpp, branch gt4gs/live).
//
// Modes:
//   gt4-gs.exe --ipc <session> --parent <pid> [--config <ini>]   live: consume the game's GS stream over shared memory
//   gt4-gs.exe --replay-gscap <file.gscap> [-dumpdir <dir>]       offline: replay a .gscap version 2 stream file
//   gt4-gs.exe [GSRunner options] <dump.gs>                        GSRunner's GS dump replay (unchanged)
//
// Live mode: no input, no audio, no memory cards; the serial (SCUS-97328 for GT4) comes from the handshake so the
// GameDB GS fixes apply; blending accuracy defaults to High (the GameDB only recommends it). The GS presents on a
// window of this process that becomes a WS_CHILD of the game's window on CONTROL attach_window; the window thread
// only pumps messages and never takes focus. After SetParent the window thread detaches its input queue from the
// game's window thread (cross-process SetParent attaches them), so a stalled GS window thread cannot hold up the
// game's keyboard or mouse input. With detached queues a click on the picture makes this thread the foreground thread
// (it has no active window, so the game window was left inactive in 0.3.0; a disabled child does the same). In 0.3.1
// every click posts a message to this thread, which then gives the foreground back to the game window (this thread
// received the click, so Windows lets it), and the child returns MA_NOACTIVATE (task 038). The child no longer
// answers WM_NCHITTEST with HTTRANSPARENT: across threads that only discarded the click (after the foreground switch).
// Folders: in the stream modes the data root is [GT4GS] DataRoot (default %LOCALAPPDATA%\GT4Recomp\gt4-gs), never the
// PCSX2 user folder, and the [Folders] keys of the --config file (Cache, Textures, ...) are applied (task 037).
// gt4-gs 1.0.0: the --config file is checked against the GS settings allow-list of the game repository (embedded at build
// time): a key or value outside it is ignored with one warning line, never a refusal to start. The window thread hides
// the cursor over the picture with the game's policy (protocol 1.2 CONTROL cursor, task 060), and GSStreamOverlay draws
// the "Compiling shaders" notice and the performance overlay (tasks 061 and 062).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef __APPLE__
#include "MacPresentation.h"
#endif
#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GSStreamOverlay.h"
#include "pcsx2/GSStreamReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "svnrev.h"

#include "StreamTransport.h"
#include "gs_stream_protocol.h"

// Down here because X11 has a lot of defines that can conflict
#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace GSRunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
	static void DumpStats();

	static bool CreatePlatformWindow();
	static void DestroyPlatformWindow();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void PumpPlatformMessages(bool forever = false);
	static void StopPlatformMessagePump();

	// GT4Recomp
	static bool ApplyGT4Config();
	static bool PrepareStream();
	static bool AttachToGameWindow(u64 parent_window, u32 width, u32 height);
	static void ResizeGameChildWindow(u32 width, u32 height);
	static void SetGameCursorPolicy(u64 flags, u32 hide_delay_ms);
} // namespace GSRunner

static constexpr u32 WINDOW_WIDTH = 640;
static constexpr u32 WINDOW_HEIGHT = 480;

static MemorySettingsInterface s_settings_interface;

static std::string s_output_prefix;

// GT4Recomp: stream modes.
enum class StreamMode
{
	None, // GSRunner dump replay
	Live, // --ipc
	File, // --replay-gscap
};
static StreamMode s_stream_mode = StreamMode::None;
static std::string s_ipc_name;
static u32 s_parent_pid = 0;
static std::string s_config_path;
static std::string s_gscap_path;
static bool s_live_surfaceless = false;
static u32 s_dump_every = 1; // stream modes: frame dumps only every N vsyncs ([GT4GS] DumpEvery)
#ifdef GT4GS_NO_JIT
static constexpr const char* GT4GS_VERSION = "gt4gs 1.2.1 (PCSX2 v2.8.2, no JIT)";
#else
static constexpr const char* GT4GS_VERSION = "gt4gs 1.2.1 (PCSX2 v2.8.2)";
#endif
// gt4-gs 1.1.0: the GT4 display crop in native PS2 pixels (left, top, right, bottom). GT4's display rectangle ends with
// one column and one row that are not part of the picture (a line along the right and the bottom edge at every
// internal resolution). gt4-gs 1.2.0: the bottom crop is 2 rows (a second bottom row showed as a line), the right stays 1.
// Fixed: the [EmuCore/GS] Crop* keys are refused even if a later allow-list lists them (see s_fixed_keys).
static constexpr int GT4_DISPLAY_CROP[4] = {0, 0, 1, 2};
// --surface-size <W>x<H> (offline replay only, for tests): a surfaceless run presents into a virtual window of this size,
// so frame dumps ([EmuCore/GS] ScreenshotSize = window resolution, the default) show the presentation rectangle (aspect,
// pillarbox, fill) as a real window would. 0 x 0 (the default) keeps the internal-resolution dumps.
static u32 s_surface_width = 0;
static u32 s_surface_height = 0;
// [GT4GS] ProhibitDynamicCode (JIT-free build only, default true): Windows Arbitrary Code Guard for this process.
static bool s_prohibit_dynamic_code = true;
static bool s_data_root_from_config = false;
static u32 s_spin_us = 0; // [GT4GS] SpinUs: transport spin before sleeping (0: the transport default, 30 us)
static u32 s_debug_stall_window_ms = 0; // [GT4GS] DebugStallWindowMs: test only, window thread stall after attach
static u32 s_debug_stall_after_ms = 0; // [GT4GS] DebugStallAfterMs: delay from attach to that stall
static s32 s_loop_count = 1;
static std::optional<bool> s_use_window;
static bool s_no_console = false;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;
static double s_last_internal_draws = 0;
static double s_last_draws = 0;
static double s_last_render_passes = 0;
static double s_last_barriers = 0;
static double s_last_copies = 0;
static double s_last_uploads = 0;
static double s_last_readbacks = 0;
static double s_last_depth_copies_rov = 0;
static double s_last_draws_rov = 0;
static double s_last_barriers_rov = 0;
static u64 s_total_internal_draws = 0;
static u64 s_total_draws = 0;
static u64 s_total_render_passes = 0;
static u64 s_total_barriers = 0;
static u64 s_total_copies = 0;
static u64 s_total_uploads = 0;
static u64 s_total_readbacks = 0;
static u64 s_total_copies_rov = 0;
static u64 s_total_draws_rov = 0;
static u64 s_total_barriers_rov = 0;
static u32 s_total_frames = 0;
static u32 s_total_drawn_frames = 0;

static bool s_perf_enable = false;
static float s_perf_updates = 0.0f;
static float s_perf_sum_fps = 0.0f;
static float s_perf_sum_internal_fps = 0.0f;
static float s_perf_sum_cpu_thread_usage = 0.0f;
static float s_perf_sum_cpu_thread_time = 0.0f;
static float s_perf_sum_gs_thread_usage = 0.0f;
static float s_perf_sum_gs_thread_time = 0.0f;
static float s_perf_sum_gpu_time = 0.0f;
static float s_perf_sum_gpu_usage = 0.0f;

bool GSRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
		return false;

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	// don't provide an ini path, or bother loading. we'll store everything in memory.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	// GT4Recomp (gt4-gs 1.0.1): VMManager::Internal::LoadStartupSettings() creates the user folders (bios, memcards, logs,
	// ...) and opens logs/emulog.txt under the data root. It runs in main() once the data root is known: for the
	// stream modes that is [GT4GS] DataRoot (ApplyStreamFolders), never the PCSX2 user folder Documents\PCSX2, which
	// 1.0.0 still created and wrote emulog.txt into.
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	// noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	if (s_loop_number == 0 && !s_output_prefix.empty() && (s_dump_every <= 1 || (s_dump_frame_number % s_dump_every) == 0))
	{
		// when we wrap around, don't race other files
		GSJoinSnapshotThreads();

		// queue dumping of this frame
		std::string dump_path(fmt::format("{}_frame{:05}.png", s_output_prefix, s_dump_frame_number));
		GSQueueSnapshot(dump_path);
	}

	if (GSIsHardwareRenderer())
	{
		const u32 last_draws = s_total_internal_draws;
		const u32 last_uploads = s_total_uploads;

		static constexpr auto update_stat = [](GSPerfMon::counter_t counter, u64& dst, double& last) {
			// perfmon resets every 30 frames to zero
			const double val = g_perfmon.GetCounter(counter);
			dst += static_cast<u64>((val < last) ? val : (val - last));
			last = val;
		};

		update_stat(GSPerfMon::Draw, s_total_internal_draws, s_last_internal_draws);
		update_stat(GSPerfMon::DrawCalls, s_total_draws, s_last_draws);
		update_stat(GSPerfMon::RenderPasses, s_total_render_passes, s_last_render_passes);
		update_stat(GSPerfMon::Barriers, s_total_barriers, s_last_barriers);
		update_stat(GSPerfMon::TextureCopies, s_total_copies, s_last_copies);
		update_stat(GSPerfMon::TextureUploads, s_total_uploads, s_last_uploads);
		update_stat(GSPerfMon::Readbacks, s_total_readbacks, s_last_readbacks);
		update_stat(GSPerfMon::TextureCopiesROV, s_total_copies_rov, s_last_depth_copies_rov);
		update_stat(GSPerfMon::DrawCallsROV, s_total_draws_rov, s_last_draws_rov);
		update_stat(GSPerfMon::BarriersROV, s_total_barriers_rov, s_last_barriers_rov);

		const bool idle_frame = s_total_frames && (last_draws == s_total_internal_draws && last_uploads == s_total_uploads);

		if (!idle_frame)
			s_total_drawn_frames++;

		s_total_frames++;

		std::atomic_thread_fence(std::memory_order_release);
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
	if (s_perf_enable)
	{
		s_perf_updates += 1.0f;
		s_perf_sum_fps += PerformanceMetrics::GetFPS();
		s_perf_sum_internal_fps += PerformanceMetrics::GetInternalFPS();
		s_perf_sum_cpu_thread_usage += PerformanceMetrics::GetCPUThreadUsage();
		s_perf_sum_cpu_thread_time += PerformanceMetrics::GetCPUThreadAverageTime();
		s_perf_sum_gs_thread_usage += PerformanceMetrics::GetGSThreadUsage();
		s_perf_sum_gs_thread_time += PerformanceMetrics::GetGSThreadAverageTime();
		s_perf_sum_gpu_time += PerformanceMetrics::GetGPUAverageTime();
		s_perf_sum_gpu_usage += PerformanceMetrics::GetGPUUsage();
	}
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	pxFailRel("Not implemented");
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "PCSX2 GS Runner Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [parameters] [--] [filename]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -dumpdir <dir>: Frame dump directory (will be dumped as filename_frameN.png).\n");
	std::fprintf(stderr, "  -dump [rt|tex|z|f|a|i|tr|ds|fs|hw]: Enabling dumping of render target, texture, z buffer, frame, "
		"alphas, and info (context, vertices, list of transfers), transfers images, draw stats, frame stats, HW config, respectively, per draw. Generates lots of data.\n");
	std::fprintf(stderr, "  -dumprange N[,L,B]: Start dumping from draw N (base 0), stops after L draws, and only "
		"those draws that are multiples of B (intersection of -dumprange and -dumprangef used)."
		"Defaults to 0,-1,1 (all draws). Only used if -dump used.\n");
	std::fprintf(stderr, "  -dumprangef NF[,LF,BF]: Start dumping from frame NF (base 0), stops after LF frames, "
		"and only those frames that are multiples of BF (intersection of -dumprange and -dumprangef used).\n"
		"Defaults to 0,-1,1 (all frames). Only used if -dump is used.\n");
	std::fprintf(stderr, "  -loop <count>: Loops dump playback N times. Defaults to 1. 0 will loop infinitely.\n");
	std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Defaults to Auto.\n");
	std::fprintf(stderr, "  -swthreads <threads>: Sets the number of threads for the software renderer.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window.\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  -perf: Enable frame timing performance stats.\n");
	std::fprintf(stderr, "  --ipc <name> --parent <pid>: GT4Recomp live mode (GS stream shared-memory session).\n");
	std::fprintf(stderr, "  --config <ini>: GT4Recomp settings ([EmuCore/GS], [Folders], [GT4GS] Surfaceless, DumpDir, DumpEvery).\n");
	std::fprintf(stderr, "  --replay-gscap <file>: replays a GT4Recomp .gscap version 2 stream file.\n");
	std::fprintf(stderr, "  --surface-size <W>x<H>: with --replay-gscap and no window: frame dumps at this virtual window size.\n");
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

namespace
{
	// GSStreamReplayer::Source over the live shared-memory session.
	class LiveSource final : public GSStreamReplayer::Source
	{
	public:
		explicit LiveSource(std::unique_ptr<gt4gs::LiveTransport> transport)
			: m_transport(std::move(transport))
		{
		}
		bool IsLive() const override { return true; }
		std::string Serial() const override { return m_transport->Serial(); }
		GSStreamReplayer::NextResult Next(GSStreamReplayer::Record& record, u64* wait_us) override
		{
			gt4gs::RawRecord raw;
			switch (m_transport->Next(&raw, wait_us))
			{
				case gt4gs::NextStatus::record:
					record.type = raw.type;
					record.flags = raw.flags;
					record.payload_bytes = raw.payload_bytes;
					record.sequence = raw.sequence;
					record.payload = raw.payload;
					return GSStreamReplayer::NextResult::Record;
				case gt4gs::NextStatus::end: return GSStreamReplayer::NextResult::End;
				case gt4gs::NextStatus::peer_exited: return GSStreamReplayer::NextResult::PeerExited;
				default: return GSStreamReplayer::NextResult::Failed;
			}
		}
		void Release() override { m_transport->Release(); }
		const char* LastError() const override { return m_transport->LastError().c_str(); }
		void CompleteVsync() override { m_transport->CompleteVsync(); }
		u8* ResponseArea(u64* capacity) override { return m_transport->ResponseArea(capacity); }
		void CompleteReadback(u64 request_id, u64 bytes, u32 status) override { m_transport->CompleteReadback(request_id, bytes, status); }
		void PostEvent(u16 type, const void* payload, u32 bytes) override { m_transport->PostEvent(type, payload, bytes); }
		void SetStateBits(u32 bits) override { m_transport->SetStateBits(bits); }

	private:
		std::unique_ptr<gt4gs::LiveTransport> m_transport;
	};

	// GSStreamReplayer::Source over a .gscap version 2 file.
	class FileSource final : public GSStreamReplayer::Source
	{
	public:
		explicit FileSource(std::unique_ptr<gt4gs::FileTransport> transport)
			: m_transport(std::move(transport))
		{
		}
		bool IsLive() const override { return false; }
		std::string Serial() const override { return m_transport->Serial(); }
		GSStreamReplayer::NextResult Next(GSStreamReplayer::Record& record, u64* wait_us) override
		{
			*wait_us = 0;
			gt4gs::RawRecord raw;
			switch (m_transport->Next(&raw))
			{
				case gt4gs::NextStatus::record:
					record.type = raw.type;
					record.flags = raw.flags;
					record.payload_bytes = raw.payload_bytes;
					record.sequence = raw.sequence;
					record.payload = raw.payload;
					return GSStreamReplayer::NextResult::Record;
				case gt4gs::NextStatus::end: return GSStreamReplayer::NextResult::End;
				default: return GSStreamReplayer::NextResult::Failed;
			}
		}
		void Release() override {}
		const char* LastError() const override { return m_transport->LastError().c_str(); }

	private:
		std::unique_ptr<gt4gs::FileTransport> m_transport;
	};
} // namespace

// gt4-gs 1.0.0: the allow-list of the game repository's settings table (docs/launcher/gs-settings-allowlist.json,
// vendored by hash and turned into this table at configure time by pcsx2-gt4gs/CMakeLists.txt).
namespace
{
	struct AllowRule
	{
		const char* section;
		const char* key;
		const char* type; // bool, int, float, enum, string, path
		const char* values; // bool and enum: accepted values, | separated
		bool has_range; // int and float
		double minimum;
		double maximum;
	};
	constexpr AllowRule s_allow_rules[] = {
#include "gt4gs_settings_allowlist.inc"
	};

	// gt4-gs 1.1.0: values the fork accepts on top of the vendored allow-list (which is the game repository's file,
	// copied unchanged). AspectRatio "Stretch" fills the window (owner request; the game repository's settings table
	// adds it to its own list later, which makes this entry redundant, not wrong).
	struct AllowExtension
	{
		const char* section;
		const char* key;
		const char* values; // extra accepted values, | separated
	};
	constexpr AllowExtension s_allow_extensions[] = {
		{"EmuCore/GS", "AspectRatio", "Stretch"},
	};

	// Keys gt4-gs sets itself and never takes from the config file, even when an allow-list lists them.
	struct FixedKey
	{
		const char* section;
		const char* key;
		const char* why;
	};
	constexpr FixedKey s_fixed_keys[] = {
		{"EmuCore/GS", "CropLeft", "fixed by gt4-gs: the GT4 display crop"},
		{"EmuCore/GS", "CropTop", "fixed by gt4-gs: the GT4 display crop"},
		{"EmuCore/GS", "CropRight", "fixed by gt4-gs: the GT4 display crop"},
		{"EmuCore/GS", "CropBottom", "fixed by gt4-gs: the GT4 display crop"},
		// gt4-gs 1.2.0: load-only. Texture dumping is not offered (see kTextureDumpingAllowed in GSTextureReplacements.h).
		{"EmuCore/GS", "DumpReplaceableTextures", "fixed by gt4-gs: texture loading only"},
		{"EmuCore/GS", "DumpReplaceableMipmaps", "fixed by gt4-gs: texture loading only"},
		{"EmuCore/GS", "DumpTexturesWithFMVActive", "fixed by gt4-gs: texture loading only"},
	};

	const char* FixedKeyReason(std::string_view section, std::string_view key)
	{
		for (const FixedKey& fixed : s_fixed_keys)
		{
			if (section == fixed.section && key == fixed.key)
				return fixed.why;
		}
		return nullptr;
	}

	const AllowExtension* FindAllowExtension(std::string_view section, std::string_view key)
	{
		for (const AllowExtension& extension : s_allow_extensions)
		{
			if (section == extension.section && key == extension.key)
				return &extension;
		}
		return nullptr;
	}

	const AllowRule* FindAllowRule(std::string_view section, std::string_view key)
	{
		for (const AllowRule& rule : s_allow_rules)
		{
			if (section == rule.section && key == rule.key)
				return &rule;
		}
		return nullptr;
	}

	// Empty when the value is accepted, otherwise why it is not.
	std::string CheckAllowedValue(const AllowRule& rule, const std::string& value)
	{
		const std::string_view type = rule.type;
		if (value.find_first_of("\r\n") != std::string::npos)
			return "a value must be one line";
		if (type == "bool" || type == "enum")
		{
			for (const std::string_view allowed : StringUtil::SplitString(rule.values, '|', false))
			{
				if (value == allowed)
					return {};
			}
			const AllowExtension* extension = FindAllowExtension(rule.section, rule.key);
			if (extension)
			{
				for (const std::string_view allowed : StringUtil::SplitString(extension->values, '|', false))
				{
					if (value == allowed)
						return {};
				}
				return fmt::format("allowed values: {}|{}", rule.values, extension->values);
			}
			return fmt::format("allowed values: {}", rule.values);
		}
		if (type == "int" || type == "float")
		{
			std::optional<double> number;
			if (type == "int")
			{
				if (const std::optional<s64> integer = StringUtil::FromChars<s64>(value))
					number = static_cast<double>(*integer);
			}
			else
			{
				number = StringUtil::FromChars<double>(value);
			}
			if (!number.has_value())
				return fmt::format("not {} number", type == "int" ? "an integer" : "a");
			if (rule.has_range && (*number < rule.minimum || *number > rule.maximum))
				return fmt::format("allowed range: {:g} to {:g}", rule.minimum, rule.maximum);
			return {};
		}
		return {}; // string and path: any single-line text
	}

	// Section names of an INI file in file order (INISettingsInterface cannot list them).
	std::vector<std::string> IniSectionNames(const std::string& path)
	{
		std::vector<std::string> sections;
		const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
		if (!text.has_value())
			return sections;
		for (const std::string_view line : StringUtil::SplitString(*text, '\n', true))
		{
			const std::string_view trimmed = StringUtil::StripWhitespace(line);
			if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
			{
				std::string name(trimmed.substr(1, trimmed.size() - 2));
				if (std::find(sections.begin(), sections.end(), name) == sections.end())
					sections.push_back(std::move(name));
			}
		}
		return sections;
	}
} // namespace

// Reads the --config file (PCSX2 INI syntax). gt4-gs 1.0.0: every key is checked against the allow-list first; an
// unknown key or a value outside its rule is ignored with one warning line in the log (the session still starts).
// Copied sections: [EmuCore/GS] (GS settings), [EmuCore] (EnableGameFixes) and [Folders] (cache, textures, snapshots).
// [GT4GS] holds the fork's own keys: Surfaceless=true runs without a window (headless checks); DumpDir and DumpEvery
// write frame PNGs; ShaderCompileNotice and PerfOverlay* configure GSStreamOverlay.
bool GSRunner::ApplyGT4Config()
{
	if (s_config_path.empty())
	{
		GSStreamOverlay::Configure({}, s_stream_mode == StreamMode::Live ? s_parent_pid : 0);
		return true;
	}
	INISettingsInterface file_ini(s_config_path);
	if (!file_ini.Load())
	{
		Console.ErrorFmt("Unable to load settings from {}.", s_config_path);
		return false;
	}
	// Only accepted keys reach `ini`; everything below reads from it.
	MemorySettingsInterface ini;
	u32 ignored = 0;
	for (const std::string& section : IniSectionNames(s_config_path))
	{
		for (const auto& [key, value] : file_ini.GetKeyValueList(section.c_str()))
		{
			const AllowRule* rule = FindAllowRule(section, key);
			const char* fixed = FixedKeyReason(section, key);
			const std::string why = fixed ? std::string(fixed) : rule ? CheckAllowedValue(*rule, value) : std::string("unknown key");
			if (!why.empty())
			{
				// Plain stderr: the PCSX2 console is not set up until the VM starts.
				std::fprintf(stderr, "gt4-gs: ignoring [%s] %s = %s in %s (%s)\n", section.c_str(), key.c_str(),
					value.c_str(), s_config_path.c_str(), why.c_str());
				++ignored;
				continue;
			}
			ini.SetStringValue(section.c_str(), key.c_str(), value.c_str());
		}
	}
	if (ignored)
		std::fprintf(stderr, "gt4-gs: %u setting(s) of %s ignored; the session starts with the others\n", ignored,
			s_config_path.c_str());
	std::fflush(stderr);
	for (const char* section : {"EmuCore/GS", "EmuCore", "Folders"})
	{
		for (const auto& [key, value] : ini.GetKeyValueList(section))
			s_settings_interface.SetStringValue(section, key.c_str(), value.c_str());
	}
	s_live_surfaceless = ini.GetBoolValue("GT4GS", "Surfaceless", false);
	s_spin_us = static_cast<u32>(std::clamp(ini.GetIntValue("GT4GS", "SpinUs", 0), 0, 2000));
	s_prohibit_dynamic_code = ini.GetBoolValue("GT4GS", "ProhibitDynamicCode", true);
	// Live presentation: acquire the next swap chain image at the next present, not right after this one (task 037).
	GSSetDeferredSwapChainAcquire(ini.GetBoolValue("GT4GS", "DeferSwapChainAcquire", s_stream_mode == StreamMode::Live));
	s_debug_stall_window_ms = static_cast<u32>(std::clamp(ini.GetIntValue("GT4GS", "DebugStallWindowMs", 0), 0, 60000));
	s_debug_stall_after_ms = static_cast<u32>(std::clamp(ini.GetIntValue("GT4GS", "DebugStallAfterMs", 0), 0, 600000));
	const std::string data_root = ini.GetStringValue("GT4GS", "DataRoot", "");
	if (!data_root.empty())
	{
		EmuFolders::DataRoot = Path::Canonicalize(data_root);
		s_data_root_from_config = true;
	}
	// Headless checks: PNG frame dumps named <DumpDir>/live_frameNNNNN.png (frame = stream VSYNC count), every
	// DumpEvery vsyncs. The same naming as GSRunner's -dumpdir, so live and offline frames compare directly.
	const std::string dump_dir = ini.GetStringValue("GT4GS", "DumpDir", "");
	s_dump_every = std::max(1, ini.GetIntValue("GT4GS", "DumpEvery", 1));
	if (!dump_dir.empty() && s_output_prefix.empty())
	{
		if (!FileSystem::DirectoryExists(dump_dir.c_str()) && !FileSystem::CreateDirectoryPath(dump_dir.c_str(), false))
		{
			Console.ErrorFmt("Cannot create the dump directory {}.", dump_dir);
			return false;
		}
		s_output_prefix = Path::Combine(dump_dir, s_stream_mode == StreamMode::Live ? "live" : "replay");
		Console.WriteLnFmt("Frame dumps: {}_frameNNNNN.png every {} vsyncs", s_output_prefix, s_dump_every);
	}
	// Tasks 061/062: the shader compile notice and the performance overlay (GSStreamOverlay.h).
	GSStreamOverlay::Config overlay;
	overlay.shader_notice = ini.GetBoolValue("GT4GS", "ShaderCompileNotice", true);
	overlay.overlay = ini.GetBoolValue("GT4GS", "PerfOverlay", false);
	overlay.fps = ini.GetBoolValue("GT4GS", "PerfOverlayFPS", true);
	overlay.frame_time = ini.GetBoolValue("GT4GS", "PerfOverlayFrameTime", true);
	overlay.cpu = ini.GetBoolValue("GT4GS", "PerfOverlayCPU", true);
	overlay.gpu = ini.GetBoolValue("GT4GS", "PerfOverlayGPU", true);
	overlay.resolution = ini.GetBoolValue("GT4GS", "PerfOverlayResolution", true);
	overlay.breakdown = ini.GetBoolValue("GT4GS", "PerfOverlayBreakdown", true);
	GSStreamOverlay::Configure(overlay, s_stream_mode == StreamMode::Live ? s_parent_pid : 0);
	Console.WriteLnFmt("Loaded settings from {}{}", s_config_path, s_live_surfaceless ? " (surfaceless)" : "");
	return true;
}

// Stream modes: a data root of our own (never the PCSX2 user folder) and the [Folders] keys from the config, applied
// before the GS opens so the shader and pipeline caches are read from and written to the configured Cache folder.
static bool ApplyStreamFolders()
{
	if (!s_data_root_from_config)
	{
		// No [GT4GS] DataRoot: %LOCALAPPDATA%\GT4Recomp\gt4-gs.
		const char* local = std::getenv("LOCALAPPDATA");
		if (!local || !*local)
		{
			Console.Error("LOCALAPPDATA is not set and the config has no [GT4GS] DataRoot.");
			return false;
		}
		EmuFolders::DataRoot = Path::Combine(Path::Combine(local, "GT4Recomp"), "gt4-gs");
	}
	if (!FileSystem::DirectoryExists(EmuFolders::DataRoot.c_str()) &&
		!FileSystem::CreateDirectoryPath(EmuFolders::DataRoot.c_str(), false))
	{
		Console.ErrorFmt("Cannot create the gt4-gs data folder {}.", EmuFolders::DataRoot);
		return false;
	}
	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);
	EmuFolders::LoadConfig(s_settings_interface);
	EmuFolders::EnsureFoldersExist();
	// Plain stderr: the PCSX2 console is not set up again until the VM starts.
	std::fprintf(stderr, "gt4-gs: data root %s, cache %s, textures %s\n", EmuFolders::DataRoot.c_str(),
		EmuFolders::Cache.c_str(), EmuFolders::Textures.c_str());
	std::fflush(stderr);
	return true;
}

// Opens the live session or the stream file and hands it to GSStreamReplayer (before VMManager::Initialize).
bool GSRunner::PrepareStream()
{
	const int renderer = s_settings_interface.GetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
	const float scale = s_settings_interface.GetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f);
	const std::string renderer_name = fmt::format("PCSX2 GS {} x{:g}",
		Pcsx2Config::GSOptions::GetRendererName(static_cast<GSRendererType>(renderer)), scale);

	GSStreamReplayer::WindowHooks hooks;
	std::string error;
	GSStreamReplayer::Source* source = nullptr;
	if (s_stream_mode == StreamMode::Live)
	{
		std::unique_ptr<gt4gs::LiveTransport> transport = gt4gs::LiveTransport::Open(s_ipc_name, s_parent_pid, &error);
		if (!transport)
		{
			Console.ErrorFmt("GS stream session {} refused: {}", s_ipc_name, error);
			return false;
		}
		Console.WriteLnFmt("GS stream session {}: serial {}, frames in flight {}, protocol {} (consumer 1.{})", s_ipc_name,
			transport->Serial(), transport->FramesInFlight(), gt4gs::ProtocolVersionText(),
			GSStreamReplayer::Protocol13::kConsumerVersionMinor);
		if (s_spin_us)
			transport->SetSpinUs(s_spin_us);
		source = new LiveSource(std::move(transport));
		hooks.attach = &GSRunner::AttachToGameWindow;
		hooks.resize = &GSRunner::ResizeGameChildWindow;
		hooks.cursor = &GSRunner::SetGameCursorPolicy;
#ifdef __APPLE__
		hooks.presentation_id = &GT4GSMac::PresentationId;
#endif
	}
	else
	{
		std::unique_ptr<gt4gs::FileTransport> transport = gt4gs::FileTransport::Open(s_gscap_path, &error);
		if (!transport)
		{
			Console.ErrorFmt("Stream file {}: {}", s_gscap_path, error);
			return false;
		}
		Console.WriteLnFmt("GS stream file {}: serial {}", s_gscap_path, transport->Serial());
		source = new FileSource(std::move(transport));
	}
	GSStreamReplayer::Prepare(source, hooks, renderer_name.c_str(), GT4GS_VERSION);
	return true;
}

void GSRunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

bool GSRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	std::string dumpdir; // Save from argument -dumpdir for creating sub-directories
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

			if (CHECK_ARG_PARAM("--ipc"))
			{
				s_stream_mode = StreamMode::Live;
				s_ipc_name = argv[++i];
				continue;
			}
			else if (CHECK_ARG_PARAM("--parent"))
			{
				s_parent_pid = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
				continue;
			}
			else if (CHECK_ARG_PARAM("--config"))
			{
				s_config_path = std::string(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(s_config_path.c_str()))
				{
					Console.ErrorFmt("Config file {} does not exist.", s_config_path);
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("--replay-gscap"))
			{
				s_stream_mode = StreamMode::File;
				s_gscap_path = std::string(StringUtil::StripWhitespace(argv[++i]));
				continue;
			}
			else if (CHECK_ARG_PARAM("--surface-size"))
			{
				const std::string_view size = StringUtil::StripWhitespace(argv[++i]);
				const size_t separator = size.find('x');
				const std::optional<u32> width = (separator == std::string_view::npos) ? std::nullopt : StringUtil::FromChars<u32>(size.substr(0, separator));
				const std::optional<u32> height = (separator == std::string_view::npos) ? std::nullopt : StringUtil::FromChars<u32>(size.substr(separator + 1));
				if (!width.has_value() || !height.has_value() || *width == 0 || *height == 0 || *width > 16384 || *height > 16384)
				{
					Console.ErrorFmt("--surface-size {} is not <W>x<H> (1 to 16384 each).", size);
					return false;
				}
				s_surface_width = *width;
				s_surface_height = *height;
				continue;
			}
			else if (CHECK_ARG("-help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG_PARAM("-dumpdir"))
			{
				dumpdir = s_output_prefix = StringUtil::StripWhitespace(argv[++i]);
				if (s_output_prefix.empty())
				{
					Console.Error("Invalid dump directory specified.");
					return false;
				}

				if (!FileSystem::DirectoryExists(s_output_prefix.c_str()) && !FileSystem::CreateDirectoryPath(s_output_prefix.c_str(), false))
				{
					Console.Error("Failed to create output directory");
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-dump"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "DumpGSData", true);

				if (str.find("rt") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveRT", true);
				if (str.find("f") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrame", true);
				if (str.find("tex") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTexture", true);
				if (str.find("z") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDepth", true);
				if (str.find("a") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveAlpha", true);
				if (str.find("i") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveInfo", true);
				if (str.find("tr") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTransferImages", true);
				if (str.find("ds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDrawStats", true);
				if (str.find("fs") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrameStats", true);
				if (str.find("hw") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveHWConfig", true);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprange"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprangef"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirhw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirsw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-loop"))
			{
				s_loop_count = StringUtil::FromChars<s32>(argv[++i]).value_or(0);
				Console.WriteLn("Looping dump playback %d times.", s_loop_count);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderer"))
			{
				const char* rname = argv[++i];

				GSRendererType type = GSRendererType::Auto;
				if (StringUtil::Strcasecmp(rname, "Auto") == 0)
					type = GSRendererType::Auto;
#ifdef _WIN32
				else if (StringUtil::Strcasecmp(rname, "dx11") == 0)
					type = GSRendererType::DX11;
				else if (StringUtil::Strcasecmp(rname, "dx12") == 0)
					type = GSRendererType::DX12;
#endif
#ifdef ENABLE_OPENGL
				else if (StringUtil::Strcasecmp(rname, "gl") == 0)
					type = GSRendererType::OGL;
#endif
#ifdef ENABLE_VULKAN
				else if (StringUtil::Strcasecmp(rname, "vulkan") == 0)
					type = GSRendererType::VK;
#endif
#ifdef __APPLE__
				else if (StringUtil::Strcasecmp(rname, "metal") == 0)
					type = GSRendererType::Metal;
#endif
				else if (StringUtil::Strcasecmp(rname, "sw") == 0)
					type = GSRendererType::SW;
				else
				{
					Console.Error("Unknown renderer '%s'", rname);
					return false;
				}

				Console.WriteLn("Using %s renderer.", Pcsx2Config::GSOptions::GetRendererName(type));
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(type));
				continue;
			}
			else if (CHECK_ARG_PARAM("-swthreads"))
			{
				const int swthreads = StringUtil::FromChars<int>(argv[++i]).value_or(0);
				if (swthreads < 0)
				{
					Console.WriteLn("Invalid number of software threads");
					return false;
				}
				
				Console.WriteLn(fmt::format("Setting number of software threads to {}", swthreads));
				s_settings_interface.SetIntValue("EmuCore/GS", "SWExtraThreads", swthreads);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderhacks"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);

				if (str.find("af") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_AutoFlushLevel", 1);
				if (str.find("cpufb") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_CPU_FB_Conversion", true);
				if (str.find("dds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", true);
				if (str.find("dpi") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", true);
				if (str.find("dsf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_Disable_Safe_Features", true);
				if (str.find("tinrt") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 1);
				if (str.find("plf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", true);

				continue;
			}
			else if (CHECK_ARG_PARAM("-ini"))
			{
				std::string path = std::string(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(path.c_str()))
				{
					Console.ErrorFmt("INI file {} does not exit.", path);
					return false;
				}

				INISettingsInterface si_ini(path);

				if (!si_ini.Load())
				{
					Console.ErrorFmt("Unable to load INI settings from {}.", path);
					return false;
				}

				for (const auto& [key, value] : si_ini.GetKeyValueList("EmuCore/GS"))
					s_settings_interface.SetStringValue("EmuCore/GS", key.c_str(), value.c_str());

				continue;
			}
			else if (CHECK_ARG_PARAM("-upscale"))
			{
				const float upscale = StringUtil::FromChars<float>(argv[++i]).value_or(0.0f);
				if (upscale < 0.5f)
				{
					Console.WriteLn("Invalid upscale multiplier");
					return false;
				}

				Console.WriteLn(fmt::format("Setting upscale multiplier to {}", upscale));
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
				continue;
			}
			else if (CHECK_ARG_PARAM("-logfile"))
			{
				const char* logfile = argv[++i];
				if (std::strlen(logfile) > 0)
				{
					// disable timestamps, since we want to be able to diff the logs
					Console.WriteLn("Logging to %s...", logfile);
					VMManager::Internal::SetFileLogPath(logfile);
					s_settings_interface.SetBoolValue("Logging", "EnableFileLogging", true);
					s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", false);
				}

				continue;
			}
			else if (CHECK_ARG("-noshadercache"))
			{
				Console.WriteLn("Disabling shader cache");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableShaderCache", true);
				continue;
			}
			else if (CHECK_ARG("-window"))
			{
				Console.WriteLn("Creating window");
				s_use_window = true;
				continue;
			}
			else if (CHECK_ARG("-surfaceless"))
			{
				Console.WriteLn("Running surfaceless");
				s_use_window = false;
				continue;
			}
			else if (CHECK_ARG("-perf"))
			{
				Console.WriteLn("Enable performance stats");
				s_perf_enable = true;
				continue;
			}
			else if (CHECK_ARG("-debugdevice"))
			{
				Console.WriteLn("Enable debug device");
				s_settings_interface.SetBoolValue("EmuCore/GS", "UseDebugDevice", true);
				continue;
			}
			else if (CHECK_ARG("--"))
			{
				no_more_args = true;
				continue;
			}
			else if (argv[i][0] == '-')
			{
				Console.Error("Unknown parameter: '%s'", argv[i]);
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!params.filename.empty())
			params.filename += ' ';
		params.filename += argv[i];
	}

	if (s_stream_mode == StreamMode::Live)
	{
		if (s_ipc_name.empty() || s_parent_pid == 0)
		{
			Console.Error("--ipc needs a session name and --parent <pid>.");
			return false;
		}
		// VMManager only needs a non-empty name; the stream source is prepared separately.
		params.filename = s_ipc_name;
		return true;
	}
	if (s_stream_mode == StreamMode::File)
	{
		if (!FileSystem::FileExists(s_gscap_path.c_str()))
		{
			Console.ErrorFmt("Stream file {} does not exist.", s_gscap_path);
			return false;
		}
		params.filename = s_gscap_path;
		if (!s_output_prefix.empty())
		{
			s_output_prefix = Path::Combine(s_output_prefix, Path::GetFileTitle(s_gscap_path));
			Console.WriteLn(fmt::format("Saving dumps as {}_frameN.png", s_output_prefix));
		}
		return true;
	}

	if (params.filename.empty())
	{
		Console.Error("No dump filename provided.");
		return false;
	}

	if (!VMManager::IsGSDumpFileName(params.filename))
	{
		Console.Error("Provided filename is not a GS dump.");
		return false;
	}

	if (s_settings_interface.GetBoolValue("EmuCore/GS", "DumpGSData") && !dumpdir.empty())
	{
		if (s_settings_interface.GetStringValue("EmuCore/GS", "HWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", dumpdir.c_str());
		if (s_settings_interface.GetStringValue("EmuCore/GS", "SWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", dumpdir.c_str());
		
		// Disable saving frames with SaveSnapshotToMemory()
		// Instead we save more "raw" snapshots when using -dump.
		s_output_prefix = "";
	}

	// set up the frame dump directory
	if (!s_output_prefix.empty())
	{
		// strip off all extensions
		std::string_view title(Path::GetFileTitle(params.filename));
		if (StringUtil::EndsWithNoCase(title, ".gs"))
			title = Path::GetFileTitle(title);

		s_output_prefix = Path::Combine(s_output_prefix, StringUtil::StripWhitespace(title));
		Console.WriteLn(fmt::format("Saving dumps as {}_frameN.png", s_output_prefix));
	}

	return true;
}

void GSRunner::SettingsOverride()
{
	// complete as quickly as possible (the live stream is paced by the game, not by a frame limiter)
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	if (s_stream_mode != StreamMode::Live)
		s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);

	// Force screenshot quality settings to something more performant, overriding any defaults good for users.
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotQuality", 10);

	// ensure all input sources are disabled, we're not using them
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);

	// we don't need any sound output (PCSX2 v2.x key; the older OutputModule key is ignored and opened a WASAPI stream)
	s_settings_interface.SetStringValue("SPU2/Output", "Backend", "Null");

	// none of the bindings are going to resolve to anything
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	// force logging
	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
	s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", true);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", true);

	// and show some stats :) (not in live mode: the player sees this window)
	const bool show_stats = (s_stream_mode != StreamMode::Live);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowFPS", show_stats);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowResolution", show_stats);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGSStats", show_stats);

	if (s_stream_mode != StreamMode::None)
	{
		// GT4Recomp defaults; the --config file can change them (applied after this function).
		s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
		s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(AccBlendLevel::High));
		s_settings_interface.SetIntValue("EmuCore/GS", "deinterlace_mode", static_cast<int>(GSInterlaceMode::Automatic));
		s_settings_interface.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
		// gt4-gs 1.1.0: the fixed GT4 display crop (native pixels; the config file cannot change it, see s_fixed_keys).
		s_settings_interface.SetIntValue("EmuCore/GS", "CropLeft", GT4_DISPLAY_CROP[0]);
		s_settings_interface.SetIntValue("EmuCore/GS", "CropTop", GT4_DISPLAY_CROP[1]);
		s_settings_interface.SetIntValue("EmuCore/GS", "CropRight", GT4_DISPLAY_CROP[2]);
		s_settings_interface.SetIntValue("EmuCore/GS", "CropBottom", GT4_DISPLAY_CROP[3]);
		std::fprintf(stderr, "gt4-gs: display crop left %d top %d right %d bottom %d (native pixels)\n", GT4_DISPLAY_CROP[0], GT4_DISPLAY_CROP[1], GT4_DISPLAY_CROP[2], GT4_DISPLAY_CROP[3]);
		// gt4-gs 1.2.0: texture loading only; dumping is off whatever the config says.
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpReplaceableTextures", false);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpReplaceableMipmaps", false);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpTexturesWithFMVActive", false);
		if (s_stream_mode == StreamMode::Live)
			s_settings_interface.SetIntValue("EmuCore/GS", "OsdMessagesPos", static_cast<int>(OsdOverlayPos::None));
	}

#ifdef GT4GS_NO_JIT
	// JIT-free build: no recompiler exists, so the configuration says so too (interpreters, no fastmem). The stream and
	// dump replayers never execute guest code; this only keeps the recompiler-dependent paths switched off.
	for (const char* key : {"EnableEE", "EnableIOP", "EnableVU0", "EnableVU1", "EnableFastmem", "EnableEECache"})
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", key, false);
#endif

	// remove memory cards, so we don't have sharing violations
	for (u32 i = 0; i < 2; i++)
	{
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}
}

void GSRunner::DumpStats()
{
	std::atomic_thread_fence(std::memory_order_acquire);
	Console.WriteLn(fmt::format("======= HW STATISTICS FOR {} ({}) FRAMES ========", s_total_frames, s_total_drawn_frames));
	Console.WriteLn(fmt::format("@HWSTAT@ Draw Calls: {} (avg {})", s_total_draws, static_cast<u64>(std::ceil(s_total_draws / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Render Passes: {} (avg {})", s_total_render_passes, static_cast<u64>(std::ceil(s_total_render_passes / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers: {} (avg {})", s_total_barriers, static_cast<u64>(std::ceil(s_total_barriers / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies: {} (avg {})", s_total_copies, static_cast<u64>(std::ceil(s_total_copies / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Uploads: {} (avg {})", s_total_uploads, static_cast<u64>(std::ceil(s_total_uploads / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Readbacks: {} (avg {})", s_total_readbacks, static_cast<u64>(std::ceil(s_total_readbacks / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies (ROV): {} (avg {})", s_total_copies_rov, static_cast<u64>(std::ceil(s_total_copies_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draws Calls (ROV): {} (avg {})", s_total_draws_rov, static_cast<u64>(std::ceil(s_total_draws_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers (ROV): {} (avg {})", s_total_barriers_rov, static_cast<u64>(std::ceil(s_total_barriers_rov / static_cast<double>(s_total_drawn_frames)))));
	if (s_perf_enable)
	{
		Console.WriteLn(fmt::format("@HWSTAT@ Minimum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMinimumFrameTime(), 1000.0f / PerformanceMetrics::GetMinimumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Average Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetAverageFrameTime(), 1000.0f / PerformanceMetrics::GetAverageFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Maximum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMaximumFrameTime(), 1000.0f / PerformanceMetrics::GetMaximumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ CPU Thread Usage: {:.3f} %", s_perf_sum_cpu_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GS Thread Usage: {:.3f} %", s_perf_sum_gs_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Usage: {:.3f} %", s_perf_sum_gpu_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average CPU Thread Time: {:.3f} ms", s_perf_sum_cpu_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GS Thread Time: {:.3f} ms", s_perf_sum_gs_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GPU Time: {:.3f} ms", s_perf_sum_gpu_time / s_perf_updates));
	}
	Console.WriteLn("============================================");
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

#ifdef GT4GS_NO_JIT
// JIT-free build (GT4GS_ONLY): nothing in this binary generates code. Windows' Arbitrary Code Guard
// (ProcessDynamicCodePolicy, ProhibitDynamicCode) makes the OS refuse any executable private memory for the rest of the
// process lifetime, so a code generator that slipped in, or a library that tries one, fails hard instead of running.
// Graphics drivers that need dynamic code in our process would fail too; [GT4GS] ProhibitDynamicCode = false is the
// switch for that case (the binary stays JIT-free; only the OS enforcement is off). The probe allocation proves the
// policy is active. Messages go to plain stderr: the PCSX2 console is not set up again until the VM starts.
static void ApplyNoJitPolicy()
{
	std::fprintf(stderr, "GT4GS no-JIT: %s, JIT-free build (no EE/IOP/VU/VIF recompilers, no x86 emitter, no GS software "
						 "JIT, no executable memory; C software rasterizer)\n", GT4GS_VERSION);
#ifdef _WIN32
	if (!s_prohibit_dynamic_code)
	{
		std::fprintf(stderr, "GT4GS no-JIT: Arbitrary Code Guard not enabled ([GT4GS] ProhibitDynamicCode = false)\n");
		std::fflush(stderr);
		return;
	}
	PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
	policy.ProhibitDynamicCode = 1;
	if (!SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &policy, sizeof(policy)))
	{
		std::fprintf(stderr, "GT4GS no-JIT: SetProcessMitigationPolicy(ProhibitDynamicCode) failed, error %lu\n",
			GetLastError());
		std::fflush(stderr);
		return;
	}
	PROCESS_MITIGATION_DYNAMIC_CODE_POLICY active = {};
	GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy, &active, sizeof(active));
	void* probe = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	const DWORD probe_error = probe ? 0 : GetLastError();
	if (probe)
	{
		VirtualFree(probe, 0, MEM_RELEASE);
		std::fprintf(stderr, "GT4GS no-JIT: Arbitrary Code Guard is set but an executable allocation still succeeded\n");
	}
	else
	{
		std::fprintf(stderr, "GT4GS no-JIT: Arbitrary Code Guard on (ProhibitDynamicCode=%u); probe RWX allocation "
							 "refused, error %lu\n", static_cast<unsigned>(active.ProhibitDynamicCode), probe_error);
	}
	std::fflush(stderr);
#endif
}

// Walks the address space and reports every committed region with an executable protection that is not part
// of a loaded image (EXE/DLL). A JIT would leave such regions behind; the JIT-free build should report none.
static void LogExecutableMemoryAudit(const char* when)
{
#ifdef _WIN32
	constexpr DWORD exec_mask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	u32 regions = 0;
	u32 exec_non_image = 0;
	MEMORY_BASIC_INFORMATION mbi;
	for (u8* addr = nullptr; VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi);
		 addr = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize)
	{
		regions++;
		if (mbi.State == MEM_COMMIT && (mbi.Protect & exec_mask) != 0 && mbi.Type != MEM_IMAGE)
		{
			exec_non_image++;
			Console.WarningFmt("GT4GS no-JIT audit ({}): executable {} region at {} size {} protect 0x{:x}", when,
				mbi.Type == MEM_PRIVATE ? "private" : "mapped", mbi.BaseAddress, mbi.RegionSize, mbi.Protect);
		}
		if (static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize < addr)
			break;
	}
	Console.WriteLnFmt("GT4GS no-JIT audit ({}): {} regions, {} executable non-image regions", when, regions,
		exec_non_image);
#endif
}
#endif

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();
		GSDumpReplayer::SetIsDumpRunner(true);

		Error boot_error;
		if (VMManager::Initialize(*params, &boot_error) == VMBootResult::StartupSuccess)
		{
			// run until end
			GSDumpReplayer::SetLoopCount(s_loop_count);
			VMManager::SetState(VMState::Running);
			if (s_perf_enable)
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
			if (s_perf_enable || s_stream_mode != StreamMode::None)
				g_gs_device->SetGPUTimingEnabled(true);
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
#ifdef GT4GS_NO_JIT
			// The GS device, its threads and the graphics driver are still alive here.
			LogExecutableMemoryAudit("end of stream, GS device open");
#endif
			VMManager::Shutdown(false);
			GSRunner::DumpStats();
			if (s_stream_mode != StreamMode::None)
				Console.WriteLn(GSStreamOverlay::Summary());
			ret->store(s_stream_mode != StreamMode::None ? GSStreamReplayer::GetExitCode() : EXIT_SUCCESS);
		}
		else
		{
			Console.ErrorFmt("Boot failed: {}", boot_error.GetDescription());
			if (s_stream_mode == StreamMode::Live)
			{
				// device_failure: the GS device or window could not be created.
				GSStreamReplayer::ReportFatal(5, fmt::format("gt4-gs could not start the GS: {}", boot_error.GetDescription()).c_str());
				ret->store(6);
			}
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	GSRunner::StopPlatformMessagePump();
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	GSRunner::InitializeConsole();

	if (!GSRunner::InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	if (!GSRunner::ParseCommandLineArgs(argc, argv, params))
		return EXIT_FAILURE;

	// GSRunner dump replay: the PCSX2 user folders as before.
	if (s_stream_mode == StreamMode::None)
		VMManager::Internal::LoadStartupSettings();

	// Override settings that shouldn't be picked up from defaults or INIs.
	GSRunner::SettingsOverride();

	if (s_stream_mode != StreamMode::None)
	{
		// GT4Recomp: the config file comes after the overrides (it sets the user's GS options), then the stream opens.
		if (!GSRunner::ApplyGT4Config() || !ApplyStreamFolders())
			return 2;
		// Folders and the log file under our data root only (see InitializeConfig).
		VMManager::Internal::LoadStartupSettings();
		if (s_stream_mode == StreamMode::Live)
			s_use_window = !s_live_surfaceless;
		else if (!s_use_window.has_value())
			s_use_window = false;
		if (!GSRunner::PrepareStream())
			return 2;
	}

#ifdef GT4GS_NO_JIT
	// Before the window, the VM and GS threads, the GS device and the graphics driver exist in this process.
	ApplyNoJitPolicy();
#endif

	if (s_use_window.value_or(true) && !GSRunner::CreatePlatformWindow())
	{
		Console.Error("Failed to create window.");
		if (s_stream_mode == StreamMode::Live)
			GSStreamReplayer::ReportFatal(5, "gt4-gs could not create its window");
		return EXIT_FAILURE;
	}

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &params, &thread_ret);
	GSRunner::PumpPlatformMessages(/*forever=*/true);
	cputhread.join();

	GSRunner::DestroyPlatformWindow();
	GSStreamOverlay::Shutdown();

#ifdef GT4GS_NO_JIT
	LogExecutableMemoryAudit("at exit");
#endif

	return thread_ret.load();
}

void Host::PumpMessagesOnCPUThread()
{
	if (s_stream_mode != StreamMode::None)
	{
		// GT4Recomp: frame dumps of a stream file are named by the stream's VSYNC count.
		MTGS::RunOnGSThread([frame_number = static_cast<u32>(GSStreamReplayer::GetFrameNumber())]() {
			s_dump_frame_number = frame_number;
			s_loop_number = 0;
		});
		return;
	}

	// update GS thread copy of frame number
	MTGS::RunOnGSThread([frame_number = GSDumpReplayer::GetFrameNumber()]() { s_dump_frame_number = frame_number; });
	MTGS::RunOnGSThread([loop_number = GSDumpReplayer::GetLoopCount()]() { s_loop_number = loop_number; });
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

//////////////////////////////////////////////////////////////////////////
// Platform specific code
//////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"PCSX2GSRunner";
static HWND s_hwnd = NULL;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool GSRunner::CreatePlatformWindow()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hIcon = NULL;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	// gt4-gs 1.2.0: black, not COLOR_WINDOW (white): the window shows black from its first frame, before the first present.
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszMenuName = NULL;
	wc.lpszClassName = WINDOW_CLASS_NAME;
	wc.hIconSm = NULL;

	if (!RegisterClassExW(&wc))
	{
		Console.Error("Window registration failed.");
		return false;
	}

	if (s_stream_mode == StreamMode::Live)
	{
		// GT4Recomp: a hidden, non-activating popup until CONTROL attach_window makes it a child of the game window.
		// The Vulkan surface is created on it, so the device has a surface from the start.
		s_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WINDOW_CLASS_NAME, L"gt4-gs",
			WS_POPUP | WS_CLIPSIBLINGS, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (!s_hwnd)
		{
			Console.Error("CreateWindowEx failed.");
			return false;
		}
		PumpPlatformMessages();
		return true;
	}

	s_hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, L"PCSX2 GS Runner",
		WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH,
		WINDOW_HEIGHT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!s_hwnd)
	{
		Console.Error("CreateWindowEx failed.");
		return false;
	}

	ShowWindow(s_hwnd, SW_SHOW);
	UpdateWindow(s_hwnd);

	// make sure all messages are processed before returning
	PumpPlatformMessages();
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (!s_hwnd)
		return;

	PumpPlatformMessages();
	DestroyWindow(s_hwnd);
	s_hwnd = {};
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_hwnd)
	{
		RECT rc = {};
		if (s_stream_mode == StreamMode::Live)
			GetClientRect(s_hwnd, &rc); // child window: the swap chain covers the client area
		else
			GetWindowRect(s_hwnd, &rc);
		wi.surface_width = static_cast<u32>(rc.right - rc.left);
		wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.window_handle = s_hwnd;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
		if (s_stream_mode == StreamMode::File && s_surface_width != 0)
		{
			// --surface-size (tests): no swap chain, but the presentation rectangle and window-size dumps use this size.
			wi.surface_width = s_surface_width;
			wi.surface_height = s_surface_height;
			wi.surface_scale = 1.0f;
		}
	}

	return wi;
}

static constexpr int SHUTDOWN_MSG = WM_APP + 0x100;
static DWORD MainThreadID;

void GSRunner::PumpPlatformMessages(bool forever)
{
	MSG msg;
	while (true)
	{
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == SHUTDOWN_MSG)
				return;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!forever)
			return;
		WaitMessage();
	}
}

void GSRunner::StopPlatformMessagePump()
{
	PostThreadMessageW(MainThreadID, SHUTDOWN_MSG, 0, 0);
}

// GT4Recomp: requests from the CPU thread, executed on the window thread (which only pumps messages).
static constexpr UINT GT4GS_ATTACH_MSG = WM_APP + 0x101; // wParam = parent HWND, lParam = MAKELPARAM(width, height)
static constexpr UINT GT4GS_RESIZE_MSG = WM_APP + 0x102; // lParam = MAKELPARAM(width, height)
static constexpr UINT GT4GS_GIVE_BACK_FOREGROUND_MSG = WM_APP + 0x103; // posted by a click on the picture
static constexpr UINT GT4GS_CURSOR_MSG = WM_APP + 0x104; // wParam = kCursorFlag* bits, lParam = hide delay in ms
static HANDLE s_attach_done = nullptr;
static std::atomic<bool> s_attach_ok{false};

static constexpr UINT_PTR GT4GS_STALL_TIMER = 0x4754;
static constexpr UINT_PTR GT4GS_CURSOR_TIMER = 0x4755;
static constexpr UINT GT4GS_CURSOR_TIMER_MS = 100;
static bool s_input_detached = false;

// Task 060 (protocol 1.2 CONTROL cursor), window thread only: the game's cursor auto-hide policy over the picture. The
// rule is the game window's (native/cursor_autohide.hpp in the game repository): hidden after the delay without
// movement while the cursor is over this window (the picture) and this window is the one under it; shown again on any
// movement; never touched elsewhere. Without a CONTROL cursor record the cursor is never changed.
static bool s_cursor_auto_hide = false;
static u32 s_cursor_delay_ms = 3000;
static bool s_cursor_has_sample = false;
static bool s_cursor_hidden = false;
static POINT s_cursor_last = {};
static ULONGLONG s_cursor_last_move_ms = 0;
static HCURSOR s_arrow_cursor = nullptr;

static void UpdateCursor(HWND hwnd)
{
	POINT point{};
	if (!s_cursor_auto_hide || !GetCursorPos(&point))
		return;
	const ULONGLONG now = GetTickCount64();
	const bool moved = !s_cursor_has_sample || point.x != s_cursor_last.x || point.y != s_cursor_last.y;
	s_cursor_has_sample = true;
	s_cursor_last = point;
	if (moved)
		s_cursor_last_move_ms = now;
	// A WS_CHILD without a border: the whole window is the picture.
	const bool over_picture = WindowFromPoint(point) == hwnd;
	const bool hide = !moved && over_picture && now >= s_cursor_last_move_ms && now - s_cursor_last_move_ms >= s_cursor_delay_ms;
	if (hide && !s_cursor_hidden)
	{
		s_cursor_hidden = true;
		SetCursor(nullptr);
		std::fprintf(stderr, "gt4-gs: cursor hidden over the picture (%u ms without movement)\n", s_cursor_delay_ms);
		std::fflush(stderr);
	}
	else if (!hide && s_cursor_hidden)
	{
		s_cursor_hidden = false;
		if (over_picture)
			SetCursor(s_arrow_cursor);
		std::fprintf(stderr, "gt4-gs: cursor shown (%s)\n", over_picture ? "moved over the picture" : "left the picture");
		std::fflush(stderr);
	}
}

static void AttachOnWindowThread(HWND parent, int width, int height)
{
	bool ok = IsWindow(parent) != FALSE;
	if (ok)
	{
		// Make the placeholder popup a child of the game window, filling its client area. The game keeps input,
		// focus, title and fullscreen handling; this window only shows the GS output.
		SetWindowLongPtrW(s_hwnd, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
		SetWindowLongPtrW(s_hwnd, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY);
		ok = SetParent(s_hwnd, parent) != nullptr || GetLastError() == ERROR_SUCCESS;
		SetWindowPos(s_hwnd, HWND_TOP, 0, 0, std::max(width, 1), std::max(height, 1),
			SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		// SetParent across processes attaches the two threads' input queues; detach them so that this thread can never
		// hold up the game's input (and the game's focus state is not shared with this thread).
		const DWORD game_thread = GetWindowThreadProcessId(parent, nullptr);
		s_input_detached = game_thread && AttachThreadInput(GetCurrentThreadId(), game_thread, FALSE) != FALSE;
		if (s_debug_stall_window_ms)
			SetTimer(s_hwnd, GT4GS_STALL_TIMER, std::max<u32>(s_debug_stall_after_ms, USER_TIMER_MINIMUM), nullptr);
	}
	s_attach_ok.store(ok);
	SetEvent(s_attach_done);
}

bool GSRunner::AttachToGameWindow(u64 parent_window, u32 width, u32 height)
{
	if (!s_hwnd)
		return false; // surfaceless
	if (!s_attach_done)
		s_attach_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	s_attach_ok.store(false);
	PostMessageW(s_hwnd, GT4GS_ATTACH_MSG, static_cast<WPARAM>(parent_window), MAKELPARAM(width, height));
	if (WaitForSingleObject(s_attach_done, 5000) != WAIT_OBJECT_0)
	{
		Console.Error("gt4-gs: the window thread did not attach within 5 s");
		return false;
	}
	Console.WriteLnFmt("gt4-gs: window attached to 0x{:X} ({}x{}): {}; input queues {}", parent_window, width, height,
		s_attach_ok.load() ? "ok" : "failed", s_input_detached ? "detached" : "still attached");
	return s_attach_ok.load();
}

void GSRunner::SetGameCursorPolicy(u64 flags, u32 hide_delay_ms)
{
	// CPU thread: hand the policy to the window thread, which owns the cursor while it is over the picture.
	if (s_hwnd)
		PostMessageW(s_hwnd, GT4GS_CURSOR_MSG, static_cast<WPARAM>(flags), static_cast<LPARAM>(hide_delay_ms));
}

void GSRunner::ResizeGameChildWindow(u32 width, u32 height)
{
	// A zero size (the game window minimized) is ignored by the replayer before it gets here.
	if (s_hwnd)
		PostMessageW(s_hwnd, GT4GS_RESIZE_MSG, 0, MAKELPARAM(width, height));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (s_stream_mode == StreamMode::Live)
	{
		switch (msg)
		{
			case GT4GS_ATTACH_MSG:
				AttachOnWindowThread(reinterpret_cast<HWND>(wParam), LOWORD(lParam), HIWORD(lParam));
				return 0;
			case GT4GS_RESIZE_MSG:
				SetWindowPos(hwnd, nullptr, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
				return 0;
			case GT4GS_CURSOR_MSG:
				s_cursor_auto_hide = (static_cast<u64>(wParam) & gs_stream::kCursorFlagAutoHide) != 0;
				s_cursor_delay_ms = static_cast<u32>(std::clamp<LPARAM>(lParam, 1, 60000));
				s_arrow_cursor = LoadCursorW(nullptr, IDC_ARROW);
				if (s_cursor_auto_hide)
				{
					SetTimer(hwnd, GT4GS_CURSOR_TIMER, GT4GS_CURSOR_TIMER_MS, nullptr);
				}
				else
				{
					KillTimer(hwnd, GT4GS_CURSOR_TIMER);
					if (s_cursor_hidden)
					{
						s_cursor_hidden = false;
						POINT point{};
						if (GetCursorPos(&point) && WindowFromPoint(point) == hwnd)
							SetCursor(s_arrow_cursor);
					}
				}
				Console.WriteLnFmt("gt4-gs: cursor auto-hide over the picture {} ({} ms without movement)",
					s_cursor_auto_hide ? "on" : "off", s_cursor_delay_ms);
				return 0;
			case WM_MOUSEMOVE:
				UpdateCursor(hwnd);
				break;
			case WM_SETCURSOR:
				// With the policy on, the picture's cursor is decided here (not by the game window through DefWindowProc).
				if (s_cursor_auto_hide && reinterpret_cast<HWND>(wParam) == hwnd && LOWORD(lParam) == HTCLIENT)
				{
					UpdateCursor(hwnd);
					SetCursor(s_cursor_hidden ? nullptr : s_arrow_cursor);
					return TRUE;
				}
				break;
			case WM_TIMER:
				if (wParam == GT4GS_CURSOR_TIMER)
				{
					UpdateCursor(hwnd);
					return 0;
				}
				if (wParam == GT4GS_STALL_TIMER)
				{
					// Test only ([GT4GS] DebugStallWindowMs): the window thread stops pumping, like a hung GS window
					// thread. The game window must stay responsive (tools/gs_live_check --stall).
					KillTimer(hwnd, GT4GS_STALL_TIMER);
					Console.WriteLnFmt("gt4-gs: debug stall of the window thread for {} ms", s_debug_stall_window_ms);
					Sleep(s_debug_stall_window_ms);
					Console.WriteLn("gt4-gs: debug stall ended");
					return 0;
				}
				break;
			case WM_MOUSEACTIVATE:
			case WM_LBUTTONDOWN:
			case WM_RBUTTONDOWN:
			case WM_MBUTTONDOWN:
			case WM_XBUTTONDOWN:
				// The click made this thread the foreground thread; give the foreground back to the game window once
				// the system has finished processing the click (posted, so it runs after the activation change).
				PostMessageW(hwnd, GT4GS_GIVE_BACK_FOREGROUND_MSG, 0, 0);
				return msg == WM_MOUSEACTIVATE ? MA_NOACTIVATE : 0;
			case GT4GS_GIVE_BACK_FOREGROUND_MSG:
			{
				const HWND root = GetAncestor(hwnd, GA_ROOT);
				const HWND before = GetForegroundWindow();
				const BOOL done = (root && root != hwnd && before != root) ? SetForegroundWindow(root) : TRUE;
				std::fprintf(stderr, "gt4-gs: click on the picture: foreground was %p, game window %p, give back %s\n",
					static_cast<void*>(before), static_cast<void*>(root), done ? "ok" : "refused");
				std::fflush(stderr);
				return 0;
			}
			case WM_SETFOCUS:
				if (HWND parent = GetParent(hwnd))
					SetFocus(parent); // never keep the keyboard focus
				return 0;
			case WM_ERASEBKGND:
			{
				// gt4-gs 1.2.0: paint black until the swap chain presents (the GT4 start used to show white here).
				RECT client_rect{};
				if (GetClientRect(hwnd, &client_rect))
					FillRect(reinterpret_cast<HDC>(wParam), &client_rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
				return 1;
			}
			default:
				break;
		}
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> u8_args;
	u8_args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; i++)
		u8_args.push_back(StringUtil::WideStringToUTF8String(argv[i]));

	std::vector<char*> u8_argptrs;
	u8_argptrs.reserve(u8_args.size());
	for (int i = 0; i < argc; i++)
		u8_argptrs.push_back(u8_args[i].data());
	u8_argptrs.push_back(nullptr);

	MainThreadID = GetCurrentThreadId();

	return real_main(argc, u8_argptrs.data());
}

#elif defined(__APPLE__)

static void* s_window;
static WindowInfo s_wi;

static bool s_live_view = false; // GT4Recomp: live mode draws into a window-less view shared with the game

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window && !s_live_view, "Tried to create window when there already was one!");
	if (s_stream_mode == StreamMode::Live)
	{
		// GT4Recomp: no window of our own; the picture layer is hosted by the game window (MacPresentation.mm).
		s_live_view = GT4GSMac::CreateLiveView(&s_wi, WINDOW_WIDTH, WINDOW_HEIGHT);
		return s_live_view;
	}
	s_window = CocoaTools::CreateWindow("PCSX2 GS Runner", WINDOW_WIDTH, WINDOW_HEIGHT);
	CocoaTools::GetWindowInfoFromWindow(&s_wi, s_window);
	PumpPlatformMessages();
	return s_window;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_live_view)
	{
		GT4GSMac::DestroyLiveView();
		s_live_view = false;
	}
	if (s_window) {
		CocoaTools::DestroyWindow(s_window);
		s_window = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window || s_live_view)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

bool GSRunner::AttachToGameWindow(u64 parent_window, u32 width, u32 height)
{
	if (!s_live_view)
		return false; // surfaceless
	const bool ok = GT4GSMac::AttachToGameWindow(std::max<u32>(width, 1), std::max<u32>(height, 1));
	if (ok)
	{
		s_wi.surface_width = std::max<u32>(width, 1);
		s_wi.surface_height = std::max<u32>(height, 1);
	}
	Console.WriteLnFmt("gt4-gs: picture layer for game window 0x{:X} ({}x{}): {} (context {})", parent_window, width,
		height, ok ? "ok" : "failed", GT4GSMac::PresentationId());
	return ok;
}

void GSRunner::SetGameCursorPolicy(u64 flags, u32 hide_delay_ms)
{
	// The game window receives the mouse over the hosted layer, so the game applies its own cursor policy there.
}

void GSRunner::ResizeGameChildWindow(u32 width, u32 height)
{
	s_wi.surface_width = width;
	s_wi.surface_height = height;
	GT4GSMac::Resize(width, height);
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	CocoaTools::RunCocoaEventLoop(forever);
}

void GSRunner::StopPlatformMessagePump()
{
	CocoaTools::StopMainThreadEventLoop();
}

#elif defined(__linux__)
static Display* s_display = nullptr;
static Window s_window = None;
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && s_window == None, "Tried to create window when there already was one!");

	s_display = XOpenDisplay(nullptr);
	if (!s_display)
	{
		Console.Error("Failed to open X11 display");
		return false;
	}

	int screen = DefaultScreen(s_display);
	Window root = RootWindow(s_display, screen);

	s_window = XCreateSimpleWindow(s_display, root, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
		BlackPixel(s_display, screen), WhitePixel(s_display, screen));

	if (s_window == None)
	{
		Console.Error("Failed to create X11 window");
		XCloseDisplay(s_display);
		s_display = nullptr;
		return false;
	}

	XStoreName(s_display, s_window, "PCSX2 GS Runner");
	XSelectInput(s_display, s_window, StructureNotifyMask);
	XMapWindow(s_display, s_window);

	s_wi.type = WindowInfo::Type::X11;
	s_wi.display_connection = s_display;
	s_wi.window_handle = reinterpret_cast<void*>(s_window);
	s_wi.surface_width = WINDOW_WIDTH;
	s_wi.surface_height = WINDOW_HEIGHT;
	s_wi.surface_scale = 1.0f;

	XFlush(s_display);
	PumpPlatformMessages();
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_display && s_window != None)
	{
		XDestroyWindow(s_display, s_window);
		s_window = None;
	}

	if (s_display)
	{
		XCloseDisplay(s_display);
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_window != None)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	do
	{
		while (XPending(s_display) > 0)
		{
			XEvent event;
			XNextEvent(s_display, &event);

			switch (event.type)
			{
				case ConfigureNotify:
				{
					const XConfigureEvent& configure = event.xconfigure;
					s_wi.surface_width = static_cast<u32>(configure.width);
					s_wi.surface_height = static_cast<u32>(configure.height);
					break;
				}
				case DestroyNotify:
					return;
				default:
					break;
			}
		}

		if (s_shutdown_requested.load())
			return;

		if (forever)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#endif // _WIN32 / __APPLE__
