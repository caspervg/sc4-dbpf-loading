/*
 * This file is part of sc4-dbpf-loading, a DLL Plugin for SimCity 4 that
 * optimizes the DBPF loading.
 *
 * Copyright (C) 2026 Casper Van Gheluwe
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, under
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#include "ModelStreamingPatch.h"
#include "FileSystem.h"
#include "Logger.h"
#include "Patcher.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <Windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// How the game loads the models after a zoom or rotation change (game version 641):
//
// cSC4ModelBackgroundLoader runs a thread that walks the city cells in a spiral around the view
// center. For every cell it calls LoadResourcesForOccupant, which reads the models and textures for
// the new zoom and rotation into the resource cache, and then appends the cell to a backlog. Each
// time the thread wakes it works until a cost budget runs out and a cRZTimer says the time slice
// is used up, then it releases the loader lock and sleeps (exemplar 0x6534284A, 0x7A4A8458,
// 0x0B579C20, property 0x0B579C22 - 65 ms by default, which Windows rounds up to its 15.6 ms timer
// tick when a mod lowers it).
//
// The main thread drains the backlog in ProcessLoadedCells, which swaps the occupants in each cell
// to their new models. Each swapped model queues a dirty rectangle, and the next frame redraws
// those rectangles of the static view, or the whole view once they cover enough of the screen.
//
// A WPR trace of a zoom change at 3840x2160 showed the loader thread doing 10-20 us of work in
// almost every wake, and the main thread spending ~60 frames of 150-600 ms each redrawing the
// static view while the cells trickled in. The loading work itself took well under a second.
//
// This patch:
// 1. Ends the loader's time slices on real elapsed time (WorkerSliceMs) instead of the game's
//    timer, and rests WorkerRestUs between slices with a high resolution timer instead of sleeping
//    a whole timer tick, while a load task is active.
// 2. Removes the backlog cap (property 0x0B579C2A, 1000 cells by default), so the loader thread
//    never waits for the main thread.
// 3. Has the main thread apply the loaded cells in batches (BatchIntervalMs), so the static view is
//    redrawn a few times per zoom change instead of every frame. Between batches the main thread
//    skips the loader's message processing, so it never waits for the loader lock.

namespace
{
	// cSC4ModelBackgroundLoader field offsets.
	constexpr uintptr_t kTaskListHeadOffset = 0x74;     // std::list<tTask> head node pointer
	constexpr uintptr_t kMessagesPendingOffset = 0x88;  // bool, set when the loader thread posted messages
	constexpr uintptr_t kBacklogBeginOffset = 0xC8;     // std::vector<SC4Point<int>> of loaded cells
	constexpr uintptr_t kBacklogEndOffset = 0xCC;
	constexpr uintptr_t kBacklogCapacityOffset = 0x17C; // uint32_t, property 0x0B579C2A

	// cRZTimer field offsets. The loader's timers use QueryPerformanceCounter ticks.
	constexpr uintptr_t kTimerStartOffset = 0x8;
	constexpr uintptr_t kTimerAccumulatedOffset = 0x10;

	// A 256x256 city has 65536 cells, the backlog holds each cell at most once per load task.
	constexpr uint32_t kUnlimitedBacklog = 1u << 20;

	// ProcessLoadedCells always applies at least one cell per call; this only guards against a
	// backlog that somehow never empties.
	constexpr int kMaxFlushPasses = 100000;

	constexpr uintptr_t ReadTunableValues_Address = 0x48CF10;
	constexpr uintptr_t ProcessMessages_Address = 0x48E170;
	constexpr uintptr_t RefocusLoad_Address = 0x48CB40;
	constexpr uintptr_t ProcessLoadedCells_Address = 0x48D800;
	constexpr uintptr_t cRZTimer_GetElapsedTime_Address = 0x890198;
	constexpr uintptr_t SleepCurrentThread_Address = 0x9205A0;

	// cSC4ModelMaker::DoMessage -> cSC4ModelBackgroundLoader::ProcessBackgroundThreadToMainThreadCommunication
	constexpr uintptr_t DoMessage_ProcessMessages_Call = 0x49619D;
	// cSC4ModelMaker view translation check -> cSC4ModelBackgroundLoader::RefocusLoadToNewViewLocation
	constexpr uintptr_t ViewTranslation_RefocusLoad_Call = 0x48EB31;
	// cSC4ModelBackgroundLoader::Init -> ReadTunableValues
	constexpr uintptr_t Init_ReadTunableValues_Call = 0x48E93B;
	// cSC4ModelBackgroundLoader::StartLoadingForZoomAndRotation -> ReadTunableValues
	constexpr uintptr_t StartLoading_ReadTunableValues_Call = 0x48DB29;
	// cSC4ModelBackgroundLoader::ProcessBackgroundThreadToMainThreadCommunication -> ProcessLoadedCells
	constexpr uintptr_t ProcessMessages_ProcessLoadedCells_Call = 0x48E1CE;
	// cSC4ModelBackgroundLoader::ProcessLoadedCells -> cRZTimer::GetElapsedTime
	constexpr uintptr_t ProcessLoadedCells_GetElapsedTime_Call = 0x48D973;
	// cSC4ModelBackgroundLoader::ThreadFunction -> cRZTimer::GetElapsedTime
	constexpr uintptr_t ThreadFunction_GetElapsedTime_Call = 0x48E4E7;
	// cSC4ModelBackgroundLoader::ThreadFunction -> cRZThread::SleepCurrentThread
	constexpr uintptr_t ThreadFunction_Sleep_Call = 0x48E877;

	typedef void(__fastcall* pfn_ReadTunableValues)(void* pLoader);
	typedef void(__fastcall* pfn_ProcessMessages)(void* pLoader);
	typedef void(__thiscall* pfn_RefocusLoad)(void* pLoader, void* pViewRect, void* pViewCenter);
	typedef bool(__fastcall* pfn_ProcessLoadedCells)(void* pLoader);
	typedef uint32_t(__thiscall* pfn_cRZTimer_GetElapsedTime)(void* pTimer);
	typedef void(__cdecl* pfn_SleepCurrentThread)(uint32_t microseconds);

	const pfn_ReadTunableValues RealReadTunableValues = reinterpret_cast<pfn_ReadTunableValues>(ReadTunableValues_Address);
	const pfn_ProcessMessages RealProcessMessages = reinterpret_cast<pfn_ProcessMessages>(ProcessMessages_Address);
	const pfn_RefocusLoad RealRefocusLoad = reinterpret_cast<pfn_RefocusLoad>(RefocusLoad_Address);
	const pfn_ProcessLoadedCells RealProcessLoadedCells = reinterpret_cast<pfn_ProcessLoadedCells>(ProcessLoadedCells_Address);
	const pfn_cRZTimer_GetElapsedTime RealGetElapsedTime = reinterpret_cast<pfn_cRZTimer_GetElapsedTime>(cRZTimer_GetElapsedTime_Address);
	const pfn_SleepCurrentThread RealSleepCurrentThread = reinterpret_cast<pfn_SleepCurrentThread>(SleepCurrentThread_Address);

	struct Settings
	{
		bool enabled = true;
		uint32_t batchIntervalMs = 250;
		uint32_t workerSliceMs = 4;
		uint32_t workerRestUs = 500;
		bool logStats = true;
	};

	Settings settings;
	int64_t qpcFrequency = 0;
	int64_t batchIntervalTicks = 0;
	uint64_t workerSliceMicroseconds = 0;

	// The loader is a singleton. Init calls ReadTunableValues on the main thread before it starts
	// the loader thread, so the thread always sees this pointer.
	std::atomic<void*> loaderInstance = nullptr;

	// Main thread state.
	bool flushingBacklog = false;
	int64_t batchStart = 0;

	struct LoadStats
	{
		int64_t firstCellTime = 0;
		uint32_t batches = 0;
		uint64_t cells = 0;
		int64_t longestBatchTicks = 0;
	};

	LoadStats loadStats;
	bool loggedBacklogCapacity = false;
	bool loggedTimerComparison = false;

	// Written once by the loader thread, logged by the main thread.
	std::atomic<bool> timerComparisonReady = false;
	uint32_t gameTimerMicroseconds = 0;
	uint64_t realTimerMicroseconds = 0;

	int64_t QpcNow()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	uint64_t TicksToMicroseconds(int64_t ticks)
	{
		return ticks <= 0 ? 0 : static_cast<uint64_t>(ticks) * 1000000 / static_cast<uint64_t>(qpcFrequency);
	}

	double TicksToMilliseconds(int64_t ticks)
	{
		return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpcFrequency);
	}

	template <typename T> T ReadField(const void* object, uintptr_t offset)
	{
		T value;
		std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(T));
		return value;
	}

	bool LoadTaskActive(const void* pLoader)
	{
		// The list head node lives as long as the loader. Reading its next pointer without the loader
		// lock is safe for this check: a 4-byte aligned load is atomic, and either value is acceptable.
		const volatile uintptr_t* head = *reinterpret_cast<volatile uintptr_t* const*>(
			static_cast<const uint8_t*>(pLoader) + kTaskListHeadOffset);

		return head != nullptr && *head != reinterpret_cast<uintptr_t>(head);
	}

	size_t BacklogSize(const void* pLoader)
	{
		const uintptr_t begin = ReadField<uintptr_t>(pLoader, kBacklogBeginOffset);
		const uintptr_t end = ReadField<uintptr_t>(pLoader, kBacklogEndOffset);

		return (end - begin) / 8;
	}

	uint64_t RealElapsedMicroseconds(const void* pTimer)
	{
		const int64_t start = ReadField<int64_t>(pTimer, kTimerStartOffset);
		const int64_t accumulated = ReadField<int64_t>(pTimer, kTimerAccumulatedOffset);

		if (start == 0)
		{
			return TicksToMicroseconds(accumulated);
		}

		return TicksToMicroseconds(accumulated + (QpcNow() - start));
	}

	void LogTimerComparison()
	{
		if (!loggedTimerComparison && timerComparisonReady.load(std::memory_order_acquire))
		{
			loggedTimerComparison = true;

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"Model streaming: the loader's timer reported %u us for %llu us of real time.",
				gameTimerMicroseconds,
				realTimerMicroseconds);
		}
	}

	void __fastcall HookedReadTunableValues(void* pLoader, void* /*edxUnused*/)
	{
		RealReadTunableValues(pLoader);

		loaderInstance.store(pLoader, std::memory_order_release);

		uint32_t* const capacity = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pLoader) + kBacklogCapacityOffset);

		if (!loggedBacklogCapacity)
		{
			loggedBacklogCapacity = true;

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"Model streaming: raised the loader backlog limit from %u to %u cells.",
				*capacity,
				(std::max)(*capacity, kUnlimitedBacklog));
		}

		*capacity = (std::max)(*capacity, kUnlimitedBacklog);
	}

	uint32_t __fastcall HookedWorkerSliceElapsedTime(void* pTimer, void* /*edxUnused*/)
	{
		const uint64_t elapsed = RealElapsedMicroseconds(pTimer);

		if (elapsed >= 2000 && !timerComparisonReady.load(std::memory_order_relaxed))
		{
			gameTimerMicroseconds = RealGetElapsedTime(pTimer);
			realTimerMicroseconds = RealElapsedMicroseconds(pTimer);
			timerComparisonReady.store(true, std::memory_order_release);
		}

		// The caller ends the slice when this value reaches the slice length from property 0x0B579C21.
		return elapsed >= workerSliceMicroseconds ? 0x7FFFFFFFu : 0u;
	}

	void RestBetweenSlices()
	{
		if (settings.workerRestUs == 0)
		{
			SwitchToThread();
			return;
		}

		// Only the loader thread calls this.
		static HANDLE timer = CreateWaitableTimerExW(
			nullptr,
			nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
			TIMER_ALL_ACCESS);

		if (timer)
		{
			LARGE_INTEGER dueTime{};
			dueTime.QuadPart = -static_cast<LONGLONG>(settings.workerRestUs) * 10; // relative, 100 ns units

			if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE))
			{
				WaitForSingleObjectEx(timer, INFINITE, TRUE);
				return;
			}
		}

		SleepEx(1, TRUE);
	}

	void __cdecl HookedWorkerSleep(uint32_t microseconds)
	{
		const void* const pLoader = loaderInstance.load(std::memory_order_acquire);

		if (!pLoader)
		{
			RealSleepCurrentThread(microseconds);
			return;
		}

		if (LoadTaskActive(pLoader))
		{
			RestBetweenSlices();
			return;
		}

		// No load task: sleep as requested, but wake early when the main thread starts a new task.
		const ULONGLONG end = GetTickCount64() + microseconds / 1000;

		while (!LoadTaskActive(pLoader))
		{
			const ULONGLONG now = GetTickCount64();

			if (now >= end)
			{
				break;
			}

			SleepEx(static_cast<DWORD>((std::min<ULONGLONG>)(end - now, 10)), TRUE);
		}
	}

	uint32_t __fastcall HookedProcessLoadedCellsElapsedTime(void* pTimer, void* /*edxUnused*/)
	{
		// ProcessLoadedCells stops once it has applied enough cells and the timer is over budget.
		// A batch applies the whole backlog in one call.
		return flushingBacklog ? 0u : RealGetElapsedTime(pTimer);
	}

	void LogLoadStats(int64_t now)
	{
		if (settings.logStats && loadStats.batches > 0)
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"Model streaming: applied %llu cells in %u batches over %.0f ms (largest batch %.1f ms).",
				loadStats.cells,
				loadStats.batches,
				TicksToMilliseconds(now - loadStats.firstCellTime),
				TicksToMilliseconds(loadStats.longestBatchTicks));
		}

		loadStats = LoadStats{};
	}

	void __fastcall HookedProcessMessages(void* pLoader, void* /*edxUnused*/)
	{
		// Runs on the main thread every frame. While a load task is active the loaded cells are left in
		// the backlog until the batch interval has passed. Skipping the call keeps the messages queued
		// and avoids waiting for the loader lock while the loader thread works.
		if (settings.logStats)
		{
			LogTimerComparison();
		}

		if (batchIntervalTicks != 0
			&& ReadField<uint8_t>(pLoader, kMessagesPendingOffset) != 0
			&& LoadTaskActive(pLoader))
		{
			const int64_t now = QpcNow();

			if (batchStart == 0)
			{
				batchStart = now;
			}

			if (loadStats.firstCellTime == 0)
			{
				loadStats.firstCellTime = now;
			}

			if (now - batchStart < batchIntervalTicks)
			{
				return;
			}
		}

		RealProcessMessages(pLoader);
	}

	bool __fastcall HookedProcessLoadedCells(void* pLoader, void* /*edxUnused*/)
	{
		// Called with the loader lock held.
		if (batchIntervalTicks == 0)
		{
			return RealProcessLoadedCells(pLoader);
		}

		const int64_t now = QpcNow();
		const bool taskActive = LoadTaskActive(pLoader);
		const size_t cells = BacklogSize(pLoader);
		bool backlogEmpty = false;

		if (loadStats.firstCellTime == 0)
		{
			loadStats.firstCellTime = now;
		}

		flushingBacklog = true;

		for (int pass = 0; pass < kMaxFlushPasses && !backlogEmpty; pass++)
		{
			backlogEmpty = RealProcessLoadedCells(pLoader);
		}

		flushingBacklog = false;

		const int64_t finished = QpcNow();

		loadStats.batches++;
		loadStats.cells += cells;
		loadStats.longestBatchTicks = (std::max)(loadStats.longestBatchTicks, finished - now);

		if (taskActive)
		{
			batchStart = finished;
		}
		else
		{
			batchStart = 0;
			LogLoadStats(finished);
		}

		return backlogEmpty;
	}

	void __fastcall HookedRefocusLoad(void* pLoader, void* /*edxUnused*/, void* pViewRect, void* pViewCenter)
	{
		// Scrolling more than 4 cells during a zoom or rotation load restarts the spiral at the new view
		// center and clears the backlog. Apply the cells that are waiting for the next batch first, so
		// they are not loaded twice.
		if (batchIntervalTicks != 0 && BacklogSize(pLoader) != 0)
		{
			RealProcessMessages(pLoader);
		}

		RealRefocusLoad(pLoader, pViewRect, pViewCenter);
	}

	struct CallSite
	{
		uintptr_t address;
		uintptr_t expectedTarget;
		void* hook;
		const char* name;
	};

	bool CallSiteTargets(uintptr_t address, uintptr_t expectedTarget)
	{
		const uint8_t* const code = reinterpret_cast<const uint8_t*>(address);

		if (code[0] != 0xE8)
		{
			return false;
		}

		int32_t displacement = 0;
		std::memcpy(&displacement, code + 1, sizeof(displacement));

		return address + 5 + static_cast<uintptr_t>(displacement) == expectedTarget;
	}

	Settings ReadSettings()
	{
		Settings result;

		const std::filesystem::path iniPath = FileSystem::GetDllIniFilePath();
		const wchar_t* const path = iniPath.c_str();
		const wchar_t* const section = L"ModelStreaming";

		result.enabled = GetPrivateProfileIntW(section, L"Enabled", 1, path) != 0;
		result.batchIntervalMs = GetPrivateProfileIntW(section, L"BatchIntervalMs", result.batchIntervalMs, path);
		result.workerSliceMs = std::clamp<uint32_t>(GetPrivateProfileIntW(section, L"WorkerSliceMs", result.workerSliceMs, path), 1, 100);
		result.workerRestUs = (std::min<uint32_t>)(GetPrivateProfileIntW(section, L"WorkerRestUs", result.workerRestUs, path), 100000);
		result.logStats = GetPrivateProfileIntW(section, L"LogStats", 1, path) != 0;

		return result;
	}
}

void ModelStreamingPatch::Install()
{
	Logger& logger = Logger::GetInstance();

	settings = ReadSettings();

	if (!settings.enabled)
	{
		logger.WriteLine(LogLevel::Info, "The model streaming patch is disabled in the INI file.");
		return;
	}

	LARGE_INTEGER frequency{};
	QueryPerformanceFrequency(&frequency);
	qpcFrequency = frequency.QuadPart;
	batchIntervalTicks = static_cast<int64_t>(settings.batchIntervalMs) * qpcFrequency / 1000;
	workerSliceMicroseconds = static_cast<uint64_t>(settings.workerSliceMs) * 1000;

	const CallSite callSites[] =
	{
		{ DoMessage_ProcessMessages_Call, ProcessMessages_Address, &HookedProcessMessages, "DoMessage" },
		{ ViewTranslation_RefocusLoad_Call, RefocusLoad_Address, &HookedRefocusLoad, "RefocusLoadToNewViewLocation" },
		{ Init_ReadTunableValues_Call, ReadTunableValues_Address, &HookedReadTunableValues, "Init" },
		{ StartLoading_ReadTunableValues_Call, ReadTunableValues_Address, &HookedReadTunableValues, "StartLoadingForZoomAndRotation" },
		{ ProcessMessages_ProcessLoadedCells_Call, ProcessLoadedCells_Address, &HookedProcessLoadedCells, "ProcessBackgroundThreadToMainThreadCommunication" },
		{ ProcessLoadedCells_GetElapsedTime_Call, cRZTimer_GetElapsedTime_Address, &HookedProcessLoadedCellsElapsedTime, "ProcessLoadedCells" },
		{ ThreadFunction_GetElapsedTime_Call, cRZTimer_GetElapsedTime_Address, &HookedWorkerSliceElapsedTime, "ThreadFunction timer" },
		{ ThreadFunction_Sleep_Call, SleepCurrentThread_Address, &HookedWorkerSleep, "ThreadFunction sleep" },
	};

	for (const CallSite& site : callSites)
	{
		if (!CallSiteTargets(site.address, site.expectedTarget))
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"Failed to install the model streaming patch: unexpected code at 0x%08X (%s).",
				site.address,
				site.name);
			return;
		}
	}

	try
	{
		for (const CallSite& site : callSites)
		{
			Patcher::InstallCallHook(site.address, site.hook);
		}

		logger.WriteLineFormatted(
			LogLevel::Info,
			"Installed the model streaming patch (batch interval %u ms, loader slices %u ms, rest %u us).",
			settings.batchIntervalMs,
			settings.workerSliceMs,
			settings.workerRestUs);
	}
	catch (const std::exception& e)
	{
		logger.WriteLineFormatted(
			LogLevel::Error,
			"Failed to install the model streaming patch: %s",
			e.what());
	}
}
