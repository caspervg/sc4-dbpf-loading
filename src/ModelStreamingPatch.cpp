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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <vector>
#include <Windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// How the game loads the models after a zoom or rotation change (game version 641):
//
// cSC4ModelBackgroundLoader runs a thread that walks the cells of the load rectangle in a spiral
// around the view center. For every cell it calls LoadResourcesForOccupant, which reads the models
// and textures for the new zoom and rotation into the resource cache, and then appends the cell to
// a backlog. The thread stops once the backlog holds property 0x0B579C2A cells (1000-5000 in the
// game's tuning exemplars 0x6534284A, 0x7A4A8458, 0x0B579C20-23) and waits for the main thread.
//
// The main thread drains the backlog in ProcessLoadedCells, which swaps the occupants in each cell
// to their new models, about 20 us per cell. Each swapped model that is on screen queues a dirty
// rectangle, and the next frame redraws those parts of the static view. cSC43DRender redraws the
// whole view instead once the rectangles cover enough of it, or once 200 model changes are queued,
// which a frame of swapped cells always reaches, even off screen. At 3840x2160 a full redraw of a
// large city costs 80-250 ms.
//
// ExpandLoadRect grows the visible area by max(0x0B579C28[zoom] cells, available RAM in MB *
// 0x0B579C29[zoom]). That was sized for 2003 hardware: with the ~4 GB a 32-bit process sees today
// the margin covers the whole city at every zoom level.
//
// So a zoom change swapped every cell of the city, a few thousand per frame, and redrew the whole
// view after each of those frames: ~60 frames of 150-600 ms in a WPR trace.
//
// This patch:
// 1. Caps the RAM term of the load margin (LoadMarginMB), so a zoom change loads the view and a
//    sensible margin around it. Scrolling past the margin streams more cells in, as the game intends.
// 2. Removes the backlog cap, ends the loader's time slices after WorkerSliceMs so it never holds the
//    loader lock for long, and rests WorkerRestUs between slices instead of a whole timer tick.
// 3. Swaps the cells within FrameBudgetMs per frame, nearest to the view center first, and holds back
//    every redraw until the cells on screen are done (VisibleProgress). The game keeps its frame rate
//    and the screen keeps its current picture meanwhile.
// 4. Redraws the view once, in TileSize tiles from the center outwards, within FrameBudgetMs per frame.
// 5. Swaps the remaining, off screen cells within FrameBudgetMs per frame without full redraws.
// 6. Makes cGZPersistResourceManager::GetPrivateResource take the resource manager lock, like every
//    other GetResource variant does. The loader thread loads through GetResource, which holds that
//    lock, while the main thread loads textures through GetPrivateResource without it, while drawing.
//    Both end up in cDBPFFile::OpenRecord, which only holds a per-file lock: the record objects come
//    from global cRZFastAllocator free lists and share reference counted buffers that are not thread
//    safe. A record handed out twice crashed the game in OpenRecord (null buffer AddRef at 0x0096DA20).
//    The game's own loader was idle most of the time, which hid this; a busy loader exposes it.

namespace
{
	// cSC4ModelBackgroundLoader field offsets.
	constexpr uintptr_t kTaskListHeadOffset = 0x74;     // std::list<tTask> head node pointer
	constexpr uintptr_t kScreenCellsOffset = 0x94;      // int, screen width in diagonal cells
	constexpr uintptr_t kBacklogBeginOffset = 0xC8;     // std::vector<SC4Point<int>> of loaded cells
	constexpr uintptr_t kBacklogEndOffset = 0xCC;
	constexpr uintptr_t kMarginMinimumOffset = 0x164;   // std::vector<uint32_t>, property 0x0B579C28
	constexpr uintptr_t kMarginPerMegabyteOffset = 0x170; // std::vector<float>, property 0x0B579C29
	constexpr uintptr_t kBacklogCapacityOffset = 0x17C; // uint32_t, property 0x0B579C2A

	// cSC43DRender field offsets.
	constexpr uintptr_t kRenderBackingStoreValidOffset = 0x65;
	constexpr uintptr_t kRenderViewportWidthOffset = 0x68;
	constexpr uintptr_t kRenderViewportHeightOffset = 0x6C;
	constexpr uintptr_t kRenderFrameCounterOffset = 0x158;
	constexpr uintptr_t kRenderDirtyQueueHeadOffset = 0x1D4;
	constexpr uintptr_t kRenderDirtyQueueFirstFrameOffset = 0x1F8;
	constexpr uintptr_t kRenderPropertiesOffset = 0x48;

	// cGZPersistResourceManager: the cRZCriticalSection that GetResource locks (vtable: Lock +0x4,
	// Unlock +0x8), and the GetPrivateResource slot of its cIGZPersistResourceManager vtable.
	constexpr uintptr_t kResourceManagerLockOffset = 0x80;
	constexpr uintptr_t ResourceManager_GetPrivateResource_Slot = 0xAD89E0;
	constexpr uintptr_t GetPrivateResource_Address = 0x972D60;

	// cRZTimer field offsets. The loader's timers use QueryPerformanceCounter ticks.
	constexpr uintptr_t kTimerStartOffset = 0x8;
	constexpr uintptr_t kTimerAccumulatedOffset = 0x10;

	// A 256x256 city has 65536 cells, the backlog holds each cell at most once per load task.
	constexpr uint32_t kUnlimitedBacklog = 1u << 20;

	// cISC43DRender::ViewUpdateReason the dirty rectangle queue uses for a removed model.
	constexpr int32_t kPartialViewUpdateReason = 7;

	constexpr uintptr_t ReadTunableValues_Address = 0x48CF10;
	constexpr uintptr_t ProcessMessages_Address = 0x48E170;
	constexpr uintptr_t StartLoading_Address = 0x48DAE0;
	constexpr uintptr_t RefocusLoad_Address = 0x48CB40;
	constexpr uintptr_t ProcessLoadedCells_Address = 0x48D800;
	constexpr uintptr_t cRZTimer_GetElapsedTime_Address = 0x890198;
	constexpr uintptr_t cRZVersion_GetSystemMemory_Address = 0x89157C;
	constexpr uintptr_t SleepCurrentThread_Address = 0x9205A0;
	constexpr uintptr_t ClearDirtyRectangleQueue_Address = 0x7C6D80;
	constexpr uintptr_t DoFullViewUpdate_Address = 0x7C7310;
	constexpr uintptr_t DoPartialViewUpdate_Address = 0x7CA9C0;
	constexpr uintptr_t ProcessDirtyRectangleQueue_Address = 0x7CAE00;

	// cSC4ModelMaker::DoMessage -> cSC4ModelBackgroundLoader::ProcessBackgroundThreadToMainThreadCommunication
	constexpr uintptr_t DoMessage_ProcessMessages_Call = 0x49619D;
	// cSC4ModelMaker zoom and rotation change -> cSC4ModelBackgroundLoader::StartLoadingForZoomAndRotation
	constexpr uintptr_t ZoomRotationChange_StartLoading_Call = 0x495C15;
	// cSC4ModelMaker view translation change -> cSC4ModelBackgroundLoader::StartLoadingForZoomAndRotation
	constexpr uintptr_t ViewTranslation_StartLoading_Call = 0x495DF5;
	// cSC4ModelMaker view translation check -> cSC4ModelBackgroundLoader::RefocusLoadToNewViewLocation
	constexpr uintptr_t ViewTranslation_RefocusLoad_Call = 0x48EB31;
	// cSC4ModelBackgroundLoader::ExpandLoadRect -> cRZVersion::GetSystemMemory
	constexpr uintptr_t ExpandLoadRect_GetSystemMemory_Call = 0x48CA69;
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
	// cSC43DRender::AddDirtyRectangleInternal -> DoFullViewUpdate, once the dirty area is too large
	constexpr uintptr_t AddDirtyRectangle_DoFullViewUpdate_Call = 0x7C90C6;
	// cSC43DRender InsertModel, RemoveModel, ChangeModel and ModeModel -> DoFullViewUpdate, once 200
	// model changes are queued
	constexpr uintptr_t InsertModel_DoFullViewUpdate_Call = 0x7CA38E;
	constexpr uintptr_t RemoveModel_DoFullViewUpdate_Call = 0x7CA476;
	constexpr uintptr_t ChangeModel_DoFullViewUpdate_Call = 0x7C947D;
	constexpr uintptr_t ModeModel_DoFullViewUpdate_Call = 0x7C92D4;

	// cSC43DRender::Draw: the block that processes the dirty rectangle queue, replaced by a jump.
	constexpr uintptr_t Draw_DirtyQueue_Inject = 0x7CB7C1;
	constexpr uintptr_t Draw_DirtyQueue_ProcessCall = 0x7CB7EF;
	constexpr uintptr_t Draw_DirtyQueue_Continue = 0x7CB807;

	typedef void(__fastcall* pfn_ReadTunableValues)(void* pLoader);
	typedef void(__fastcall* pfn_ProcessMessages)(void* pLoader);
	typedef void(__thiscall* pfn_StartLoading)(void* pLoader, int32_t zoom, int32_t rotation, void* pLoadRect, void* pViewCenter);
	typedef void(__thiscall* pfn_RefocusLoad)(void* pLoader, void* pLoadRect, void* pViewCenter);
	typedef bool(__fastcall* pfn_ProcessLoadedCells)(void* pLoader);
	typedef uint32_t(__thiscall* pfn_cRZTimer_GetElapsedTime)(void* pTimer);
	typedef bool(__stdcall* pfn_cRZVersion_GetSystemMemory)(uint32_t* pTotalMegabytes, uint32_t* pAvailableMegabytes);
	typedef void(__cdecl* pfn_SleepCurrentThread)(uint32_t microseconds);
	typedef void(__fastcall* pfn_ClearDirtyRectangleQueue)(void* pRender);
	typedef void(__thiscall* pfn_DoFullViewUpdate)(void* pRender, int32_t reason);
	typedef void(__thiscall* pfn_DoPartialViewUpdate)(void* pRender, const int32_t* pRect, int32_t reason);
	typedef void(__fastcall* pfn_ProcessDirtyRectangleQueue)(void* pRender);
	typedef bool(__thiscall* pfn_GetPrivateResource)(void* pResourceManager, const void* pKey, uint32_t riid, void** ppvObj, uint32_t unknown1, void* unknown2);
	typedef void(__thiscall* pfn_LockMethod)(void* pLock);

	const pfn_ReadTunableValues RealReadTunableValues = reinterpret_cast<pfn_ReadTunableValues>(ReadTunableValues_Address);
	const pfn_ProcessMessages RealProcessMessages = reinterpret_cast<pfn_ProcessMessages>(ProcessMessages_Address);
	const pfn_StartLoading RealStartLoading = reinterpret_cast<pfn_StartLoading>(StartLoading_Address);
	const pfn_RefocusLoad RealRefocusLoad = reinterpret_cast<pfn_RefocusLoad>(RefocusLoad_Address);
	const pfn_ProcessLoadedCells RealProcessLoadedCells = reinterpret_cast<pfn_ProcessLoadedCells>(ProcessLoadedCells_Address);
	const pfn_cRZTimer_GetElapsedTime RealGetElapsedTime = reinterpret_cast<pfn_cRZTimer_GetElapsedTime>(cRZTimer_GetElapsedTime_Address);
	const pfn_cRZVersion_GetSystemMemory RealGetSystemMemory = reinterpret_cast<pfn_cRZVersion_GetSystemMemory>(cRZVersion_GetSystemMemory_Address);
	const pfn_SleepCurrentThread RealSleepCurrentThread = reinterpret_cast<pfn_SleepCurrentThread>(SleepCurrentThread_Address);
	const pfn_ClearDirtyRectangleQueue RealClearDirtyRectangleQueue = reinterpret_cast<pfn_ClearDirtyRectangleQueue>(ClearDirtyRectangleQueue_Address);
	const pfn_DoFullViewUpdate RealDoFullViewUpdate = reinterpret_cast<pfn_DoFullViewUpdate>(DoFullViewUpdate_Address);
	const pfn_DoPartialViewUpdate RealDoPartialViewUpdate = reinterpret_cast<pfn_DoPartialViewUpdate>(DoPartialViewUpdate_Address);
	const pfn_ProcessDirtyRectangleQueue RealProcessDirtyRectangleQueue = reinterpret_cast<pfn_ProcessDirtyRectangleQueue>(ProcessDirtyRectangleQueue_Address);
	const pfn_GetPrivateResource RealGetPrivateResource = reinterpret_cast<pfn_GetPrivateResource>(GetPrivateResource_Address);

	struct Settings
	{
		bool enabled = true;
		uint32_t loadMarginMB = 512;
		float visibleProgress = 2.0f;
		uint32_t frameBudgetMs = 8;
		uint32_t tileSize = 512;
		uint32_t visibleMaxMs = 2000;
		uint32_t workerSliceMs = 4;
		uint32_t workerRestUs = 500;
		bool logStats = true;
	};

	Settings settings;
	int64_t qpcFrequency = 0;
	int64_t frameBudgetTicks = 0;
	int64_t visibleMaxTicks = 0;
	uint64_t frameBudgetMicroseconds = 0;
	uint64_t workerSliceMicroseconds = 0;

	// The loader is a singleton. Init calls ReadTunableValues on the main thread before it starts
	// the loader thread, so the thread always sees this pointer.
	std::atomic<void*> loaderInstance = nullptr;

	// Main thread state for the current view change.
	enum class Phase
	{
		Idle,
		// Swapping the cells on screen, all redraws held back.
		Swap,
		// The cells on screen are swapped; the next frame picks the tiles to redraw.
		SwapDone,
		// Redrawing the view in tiles.
		Tiles,
		// Swapping the off screen rest.
		Trickle,
	};

	enum class TimerMode
	{
		Game,
		FrameBudget,
	};

	struct Rect
	{
		int32_t left;
		int32_t top;
		int32_t right;
		int32_t bottom;
	};

	Phase phase = Phase::Idle;
	TimerMode cellsTimerMode = TimerMode::Game;
	bool redrawNeeded = false;
	int32_t spiralCenterX = 0;
	int32_t spiralCenterZ = 0;
	int32_t visibleRings = 0;
	std::vector<Rect> tiles;
	size_t nextTile = 0;

	struct LoadStats
	{
		int64_t start = 0;
		size_t swapCells = 0;
		uint32_t swapFrames = 0;
		int64_t swapTicks = 0;
		int64_t longestSwapTicks = 0;
		int64_t swapEnd = 0;
		uint32_t tileCount = 0;
		uint32_t tileFrames = 0;
		int64_t tileTicks = 0;
		int64_t longestTileTicks = 0;
		size_t trickleCells = 0;
		uint32_t trickleFrames = 0;
		int64_t trickleTicks = 0;
		int64_t longestTrickleTicks = 0;
		uint32_t heldFullRedraws = 0;
	};

	LoadStats loadStats;
	bool loggedTunables = false;

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

	int32_t FrontCellRing(const void* pLoader)
	{
		// The backlog is in spiral order, so its first cell is the one nearest to the view center.
		const int32_t* const cell = ReadField<const int32_t*>(pLoader, kBacklogBeginOffset);

		return (std::max)(std::abs(cell[0] - spiralCenterX), std::abs(cell[1] - spiralCenterZ));
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

	void SetSpiral(const void* pLoader, const void* pViewCenter)
	{
		const int32_t* const center = static_cast<const int32_t*>(pViewCenter);
		spiralCenterX = center[0];
		spiralCenterZ = center[1];

		// The screen width measured along the cell diagonals. A ring of the spiral is a square, so the
		// screen corners lie further out than half the width; VisibleProgress 1.0 is half the width.
		const int32_t screenCells = ReadField<int32_t>(pLoader, kScreenCellsOffset);
		visibleRings = static_cast<int32_t>(std::ceil(settings.visibleProgress * (screenCells + 1) * 0.5f));
	}

	void LogViewChange(int64_t now)
	{
		if (!settings.logStats || (loadStats.swapCells == 0 && loadStats.trickleCells == 0))
		{
			return;
		}

		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Info,
			"Model streaming: %u visible cells in %u frames (%.0f ms, max %.1f ms), %u tiles in %u frames "
			"(%.0f ms, max %.1f ms), %u more cells in %u frames (%.0f ms, max %.1f ms); %u full redraws held, "
			"visible after %.0f ms, done after %.0f ms (spiral limit %d rings).",
			static_cast<uint32_t>(loadStats.swapCells),
			loadStats.swapFrames,
			TicksToMilliseconds(loadStats.swapTicks),
			TicksToMilliseconds(loadStats.longestSwapTicks),
			loadStats.tileCount,
			loadStats.tileFrames,
			TicksToMilliseconds(loadStats.tileTicks),
			TicksToMilliseconds(loadStats.longestTileTicks),
			static_cast<uint32_t>(loadStats.trickleCells),
			loadStats.trickleFrames,
			TicksToMilliseconds(loadStats.trickleTicks),
			TicksToMilliseconds(loadStats.longestTrickleTicks),
			loadStats.heldFullRedraws,
			TicksToMilliseconds((loadStats.swapEnd != 0 ? loadStats.swapEnd : now) - loadStats.start),
			TicksToMilliseconds(now - loadStats.start),
			visibleRings);
	}

	void StartViewChange()
	{
		if (phase != Phase::Idle)
		{
			LogViewChange(QpcNow());
		}

		phase = Phase::Swap;
		redrawNeeded = false;
		tiles.clear();
		nextTile = 0;
		loadStats = LoadStats{};
		loadStats.start = QpcNow();
	}

	void FinishViewChange()
	{
		LogViewChange(QpcNow());
		phase = Phase::Idle;
		loadStats = LoadStats{};
	}

	void EndSwapPhase(int64_t now)
	{
		loadStats.swapEnd = now;
		phase = Phase::SwapDone;
		tiles.clear();
		nextTile = 0;
	}

	void LogTunables(const void* pLoader, uint32_t originalCapacity)
	{
		// ExpandLoadRect: margin = max(minimum[zoom], available MB * perMegabyte[zoom]).
		const uint32_t* const minimum = ReadField<const uint32_t*>(pLoader, kMarginMinimumOffset);
		const float* const perMegabyte = ReadField<const float*>(pLoader, kMarginPerMegabyteOffset);

		MEMORYSTATUS memory{ sizeof(memory) };
		GlobalMemoryStatus(&memory);
		const uint32_t available = static_cast<uint32_t>(memory.dwAvailPhys >> 20);
		const uint32_t capped = settings.loadMarginMB != 0 ? (std::min)(available, settings.loadMarginMB) : available;

		uint32_t game[5]{};
		uint32_t patched[5]{};

		if (minimum && perMegabyte)
		{
			for (int zoom = 0; zoom < 5; zoom++)
			{
				game[zoom] = (std::max)(minimum[zoom], static_cast<uint32_t>(available * perMegabyte[zoom]));
				patched[zoom] = (std::max)(minimum[zoom], static_cast<uint32_t>(capped * perMegabyte[zoom]));
			}
		}

		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Info,
			"Model streaming: load margins %u/%u/%u/%u/%u cells (game: %u/%u/%u/%u/%u for %u MB free), "
			"backlog limit %u -> %u cells.",
			patched[0], patched[1], patched[2], patched[3], patched[4],
			game[0], game[1], game[2], game[3], game[4],
			available,
			originalCapacity,
			(std::max)(originalCapacity, kUnlimitedBacklog));
	}

	// Loader thread side.

	void __fastcall HookedReadTunableValues(void* pLoader, void* /*edxUnused*/)
	{
		RealReadTunableValues(pLoader);

		loaderInstance.store(pLoader, std::memory_order_release);

		uint32_t* const capacity = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pLoader) + kBacklogCapacityOffset);

		if (!loggedTunables)
		{
			loggedTunables = true;
			LogTunables(pLoader, *capacity);
		}

		*capacity = (std::max)(*capacity, kUnlimitedBacklog);
	}

	bool __fastcall HookedGetSystemMemory(
		void* /*ecxUnused*/,
		void* /*edxUnused*/,
		uint32_t* pTotalMegabytes,
		uint32_t* pAvailableMegabytes)
	{
		const bool result = RealGetSystemMemory(pTotalMegabytes, pAvailableMegabytes);

		// Only ExpandLoadRect calls this copy: it multiplies the available memory by property
		// 0x0B579C29 to size the load margin.
		if (result && settings.loadMarginMB != 0 && pAvailableMegabytes)
		{
			*pAvailableMegabytes = (std::min)(*pAvailableMegabytes, settings.loadMarginMB);
		}

		return result;
	}

	uint32_t __fastcall HookedWorkerSliceElapsedTime(void* pTimer, void* /*edxUnused*/)
	{
		// The loader thread holds the loader lock for its whole slice, and the main thread takes that
		// lock every frame while a load runs. The game's slices are 15-40 ms (property 0x0B579C21).
		const uint64_t elapsed = RealElapsedMicroseconds(pTimer);

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

	// Resource manager.

	bool __fastcall HookedGetPrivateResource(
		void* pResourceManager,
		void* /*edxUnused*/,
		const void* pKey,
		uint32_t riid,
		void** ppvObj,
		uint32_t unknown1,
		void* unknown2)
	{
		// The same lock GetResource holds while it loads. It is recursive, so resource factories that
		// load other resources keep working.
		void* const pLock = static_cast<uint8_t*>(pResourceManager) + kResourceManagerLockOffset;
		const pfn_LockMethod* const lockVTable = *static_cast<const pfn_LockMethod* const*>(pLock);

		lockVTable[1](pLock);
		const bool result = RealGetPrivateResource(pResourceManager, pKey, riid, ppvObj, unknown1, unknown2);
		lockVTable[2](pLock);

		return result;
	}

	// Main thread side: swapping the loaded cells.

	uint32_t __fastcall HookedProcessLoadedCellsElapsedTime(void* pTimer, void* /*edxUnused*/)
	{
		// ProcessLoadedCells stops once it has applied property 0x0B579C24 cells and this timer passes
		// property 0x0B579C23 (up to 3 s in the game's tuning).
		if (cellsTimerMode == TimerMode::FrameBudget)
		{
			return RealElapsedMicroseconds(pTimer) >= frameBudgetMicroseconds ? 0x7FFFFFFFu : 0u;
		}

		return RealGetElapsedTime(pTimer);
	}

	void __fastcall HookedStartLoading(
		void* pLoader,
		void* /*edxUnused*/,
		int32_t zoom,
		int32_t rotation,
		void* pLoadRect,
		void* pViewCenter)
	{
		// Clears the backlog and the tasks and starts a new spiral at the view center.
		RealStartLoading(pLoader, zoom, rotation, pLoadRect, pViewCenter);
		StartViewChange();
		SetSpiral(pLoader, pViewCenter);
	}

	void __fastcall HookedRefocusLoad(void* pLoader, void* /*edxUnused*/, void* pLoadRect, void* pViewCenter)
	{
		// Scrolling more than 4 cells during a load clears the backlog and restarts the spiral at the
		// new view center. The new spiral revisits the cells that were not applied yet.
		RealRefocusLoad(pLoader, pLoadRect, pViewCenter);

		if (phase != Phase::Idle)
		{
			SetSpiral(pLoader, pViewCenter);

			if (phase == Phase::Tiles)
			{
				// The scroll moved the picture the tiles were redrawing.
				redrawNeeded = true;
			}

			phase = Phase::Swap;
			loadStats.start = QpcNow();
		}
	}

	void __fastcall HookedProcessMessages(void* pLoader, void* /*edxUnused*/)
	{
		// Runs on the main thread every frame. While the view is redrawn in tiles no cells are swapped,
		// so the tiles get the frame budget. Skipping the call keeps the loader's messages queued.
		if (phase == Phase::Tiles)
		{
			return;
		}

		RealProcessMessages(pLoader);
	}

	bool __fastcall HookedProcessLoadedCells(void* pLoader, void* /*edxUnused*/)
	{
		// Called with the loader lock held, only when the backlog is not empty. Returning false keeps
		// the loader's messages queued, so the game calls this again next frame.
		const int64_t start = QpcNow();
		const size_t before = BacklogSize(pLoader);

		cellsTimerMode = TimerMode::FrameBudget;
		const bool backlogEmpty = RealProcessLoadedCells(pLoader);
		cellsTimerMode = TimerMode::Game;

		const int64_t finished = QpcNow();
		const int64_t ticks = finished - start;
		const size_t applied = before - BacklogSize(pLoader);
		const bool taskActive = LoadTaskActive(pLoader);

		if (phase == Phase::Swap)
		{
			loadStats.swapCells += applied;
			loadStats.swapFrames++;
			loadStats.swapTicks += ticks;
			loadStats.longestSwapTicks = (std::max)(loadStats.longestSwapTicks, ticks);

			const bool visibleDone = backlogEmpty ? !taskActive : FrontCellRing(pLoader) > visibleRings;

			if (visibleDone || finished - loadStats.start >= visibleMaxTicks)
			{
				EndSwapPhase(finished);
			}
		}
		else if (phase != Phase::Idle)
		{
			loadStats.trickleCells += applied;
			loadStats.trickleFrames++;
			loadStats.trickleTicks += ticks;
			loadStats.longestTrickleTicks = (std::max)(loadStats.longestTrickleTicks, ticks);

			if (phase == Phase::Trickle && backlogEmpty && !taskActive)
			{
				FinishViewChange();
			}
		}

		return backlogEmpty;
	}

	// Main thread side: redrawing.

	void __fastcall HookedDoFullViewUpdateForDirtyArea(void* pRender, void* /*edxUnused*/, int32_t reason)
	{
		// The queued dirty rectangles cover too much of the view. While cells are swapped, the redraw
		// waits for the tiles; afterwards a large dirty area is redrawn in tiles too.
		if (phase != Phase::Idle && ReadField<uint8_t>(pRender, kRenderBackingStoreValidOffset) != 0)
		{
			RealClearDirtyRectangleQueue(pRender);
			loadStats.heldFullRedraws++;

			redrawNeeded = true;

			if (phase == Phase::Trickle)
			{
				phase = Phase::SwapDone;
			}

			return;
		}

		RealDoFullViewUpdate(pRender, reason);
	}

	void __fastcall HookedDoFullViewUpdateForModelChanges(void* pRender, void* /*edxUnused*/, int32_t reason)
	{
		// 200 queued model changes. During a view change these are the swapped cells, most of them off
		// screen; the ones on screen still queue their dirty rectangles.
		if (phase != Phase::Idle && ReadField<uint8_t>(pRender, kRenderBackingStoreValidOffset) != 0)
		{
			loadStats.heldFullRedraws++;
			return;
		}

		RealDoFullViewUpdate(pRender, reason);
	}

	std::vector<Rect> QueuedDirtyRectangles(const void* pRender)
	{
		// std::list<DirtyRectangleQueueItem>: next, previous, then the item. The item's type is at
		// +0xC, its rectangle (left, top, right, bottom) at +0x10.
		std::vector<Rect> result;
		const uintptr_t head = ReadField<uintptr_t>(pRender, kRenderDirtyQueueHeadOffset);

		for (uintptr_t node = *reinterpret_cast<const uintptr_t*>(head);
			 node != head;
			 node = *reinterpret_cast<const uintptr_t*>(node))
		{
			result.push_back(ReadField<Rect>(reinterpret_cast<const void*>(node), 0x10));
		}

		return result;
	}

	void BuildTiles(const void* pRender, bool wholeView)
	{
		const int32_t width = ReadField<int32_t>(pRender, kRenderViewportWidthOffset);
		const int32_t height = ReadField<int32_t>(pRender, kRenderViewportHeightOffset);
		const int32_t size = static_cast<int32_t>(settings.tileSize);
		const std::vector<Rect> dirty = wholeView ? std::vector<Rect>() : QueuedDirtyRectangles(pRender);

		tiles.clear();
		nextTile = 0;

		for (int32_t top = 0; top < height; top += size)
		{
			for (int32_t left = 0; left < width; left += size)
			{
				const Rect tile{ left, top, (std::min)(left + size, width), (std::min)(top + size, height) };
				const bool touched = wholeView || std::any_of(dirty.begin(), dirty.end(), [&tile](const Rect& r)
				{
					return r.left < tile.right && tile.left < r.right && r.top < tile.bottom && tile.top < r.bottom;
				});

				if (touched)
				{
					tiles.push_back(tile);
				}
			}
		}

		// Center outwards, like the loader's spiral.
		const int32_t centerX = width / 2;
		const int32_t centerY = height / 2;

		std::sort(tiles.begin(), tiles.end(), [centerX, centerY](const Rect& a, const Rect& b)
		{
			const int64_t ax = (a.left + a.right) / 2 - centerX;
			const int64_t ay = (a.top + a.bottom) / 2 - centerY;
			const int64_t bx = (b.left + b.right) / 2 - centerX;
			const int64_t by = (b.top + b.bottom) / 2 - centerY;
			return ax * ax + ay * ay < bx * bx + by * by;
		});
	}

	void FinishTiles()
	{
		tiles.clear();
		nextTile = 0;

		const void* const pLoader = loaderInstance.load(std::memory_order_acquire);

		if (pLoader && BacklogSize(pLoader) == 0 && !LoadTaskActive(pLoader))
		{
			FinishViewChange();
		}
		else
		{
			phase = Phase::Trickle;
		}
	}

	void DrawTiles(void* pRender)
	{
		if (ReadField<uint8_t>(pRender, kRenderBackingStoreValidOffset) == 0)
		{
			// Draw just redrew the whole view.
			FinishTiles();
			return;
		}

		if (nextTile >= tiles.size())
		{
			FinishTiles();
			return;
		}

		const int64_t start = QpcNow();
		int64_t now = start;

		do
		{
			const Rect& tile = tiles[nextTile++];
			RealDoPartialViewUpdate(pRender, &tile.left, kPartialViewUpdateReason);
			now = QpcNow();
		} while (nextTile < tiles.size() && now - start < frameBudgetTicks);

		loadStats.tileFrames++;
		loadStats.tileTicks += now - start;
		loadStats.longestTileTicks = (std::max)(loadStats.longestTileTicks, now - start);

		if (nextTile >= tiles.size())
		{
			FinishTiles();
		}
	}

	void ProcessDirtyQueueAsGameDoes(void* pRender)
	{
		const uintptr_t head = ReadField<uintptr_t>(pRender, kRenderDirtyQueueHeadOffset);

		if (*reinterpret_cast<const uintptr_t*>(head) == head)
		{
			return;
		}

		// Waits render property 0x12 (DirtyRectMergeFrames) frames after the first queued rectangle.
		const uintptr_t properties = ReadField<uintptr_t>(pRender, kRenderPropertiesOffset);
		const uintptr_t intProperties = ReadField<uintptr_t>(reinterpret_cast<const void*>(properties), 0x18);
		const int32_t mergeFrames = ReadField<int32_t>(reinterpret_cast<const void*>(intProperties), 0x2EC);
		const int32_t firstFrame = ReadField<int32_t>(pRender, kRenderDirtyQueueFirstFrameOffset);
		const int32_t frame = ReadField<int32_t>(pRender, kRenderFrameCounterOffset);

		if (frame > firstFrame + mergeFrames || frame < firstFrame)
		{
			RealProcessDirtyRectangleQueue(pRender);
		}
	}

	void PrepareRedraw(void* pRender)
	{
		// The held dirty rectangles, or the whole view when they grew too large, redrawn once in tiles.
		BuildTiles(pRender, redrawNeeded);
		redrawNeeded = false;
		loadStats.tileCount += static_cast<uint32_t>(tiles.size());
		RealClearDirtyRectangleQueue(pRender);
		phase = tiles.empty() ? Phase::Trickle : Phase::Tiles;
	}

	void __cdecl DrawDirtyQueueStep(void* pRender)
	{
		if (phase == Phase::Trickle)
		{
			const void* const pLoader = loaderInstance.load(std::memory_order_acquire);

			if (pLoader && BacklogSize(pLoader) == 0 && !LoadTaskActive(pLoader))
			{
				FinishViewChange();
			}
		}
		else if (phase == Phase::Swap)
		{
			// ProcessLoadedCells ends the phase while cells arrive; this catches a load that finished
			// with an empty backlog, and the time limit.
			const int64_t now = QpcNow();
			const void* const pLoader = loaderInstance.load(std::memory_order_acquire);

			if (now - loadStats.start >= visibleMaxTicks
				|| (pLoader && BacklogSize(pLoader) == 0 && !LoadTaskActive(pLoader)))
			{
				EndSwapPhase(now);
			}
		}

		if (phase == Phase::SwapDone)
		{
			if (ReadField<uint8_t>(pRender, kRenderBackingStoreValidOffset) == 0)
			{
				// Draw just redrew the whole view.
				RealClearDirtyRectangleQueue(pRender);
				redrawNeeded = false;
				phase = Phase::Trickle;
			}
			else
			{
				PrepareRedraw(pRender);
			}
		}

		switch (phase)
		{
		case Phase::Swap:
			// Held back until the cells on screen are swapped.
			break;
		case Phase::Tiles:
			DrawTiles(pRender);
			break;
		case Phase::Idle:
		case Phase::Trickle:
		default:
			ProcessDirtyQueueAsGameDoes(pRender);
			break;
		}
	}

	void NAKED_FUN DrawDirtyQueueHook()
	{
		// Replaces 0x7CB7C1-0x7CB7F5 of cSC43DRender::Draw. ESI is the renderer; the code at
		// Draw_DirtyQueue_Continue only relies on ESI, EBX and the stack, which the call preserves.
		__asm
		{
			push esi
			call DrawDirtyQueueStep
			add esp, 4
			mov eax, Draw_DirtyQueue_Continue
			jmp eax
		}
	}

	// Installation.

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
		result.loadMarginMB = GetPrivateProfileIntW(section, L"LoadMarginMB", result.loadMarginMB, path);
		result.frameBudgetMs = std::clamp<uint32_t>(GetPrivateProfileIntW(section, L"FrameBudgetMs", result.frameBudgetMs, path), 1, 100);
		result.tileSize = std::clamp<uint32_t>(GetPrivateProfileIntW(section, L"TileSize", result.tileSize, path), 64, 4096);
		result.visibleMaxMs = std::clamp<uint32_t>(GetPrivateProfileIntW(section, L"VisibleMaxMs", result.visibleMaxMs, path), 100, 10000);
		result.workerSliceMs = std::clamp<uint32_t>(GetPrivateProfileIntW(section, L"WorkerSliceMs", result.workerSliceMs, path), 1, 100);
		result.workerRestUs = (std::min<uint32_t>)(GetPrivateProfileIntW(section, L"WorkerRestUs", result.workerRestUs, path), 100000);
		result.logStats = GetPrivateProfileIntW(section, L"LogStats", 1, path) != 0;

		wchar_t buffer[32]{};
		GetPrivateProfileStringW(section, L"VisibleProgress", L"2.0", buffer, static_cast<DWORD>(std::size(buffer)), path);
		const float visibleProgress = static_cast<float>(_wtof(buffer));
		result.visibleProgress = visibleProgress > 0.0f ? (std::min)(visibleProgress, 8.0f) : 2.0f;

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
	frameBudgetTicks = static_cast<int64_t>(settings.frameBudgetMs) * qpcFrequency / 1000;
	visibleMaxTicks = static_cast<int64_t>(settings.visibleMaxMs) * qpcFrequency / 1000;
	frameBudgetMicroseconds = static_cast<uint64_t>(settings.frameBudgetMs) * 1000;
	workerSliceMicroseconds = static_cast<uint64_t>(settings.workerSliceMs) * 1000;

	const CallSite callSites[] =
	{
		{ DoMessage_ProcessMessages_Call, ProcessMessages_Address, &HookedProcessMessages, "DoMessage" },
		{ ZoomRotationChange_StartLoading_Call, StartLoading_Address, &HookedStartLoading, "zoom and rotation change" },
		{ ViewTranslation_StartLoading_Call, StartLoading_Address, &HookedStartLoading, "view translation" },
		{ ViewTranslation_RefocusLoad_Call, RefocusLoad_Address, &HookedRefocusLoad, "RefocusLoadToNewViewLocation" },
		{ ExpandLoadRect_GetSystemMemory_Call, cRZVersion_GetSystemMemory_Address, &HookedGetSystemMemory, "ExpandLoadRect" },
		{ Init_ReadTunableValues_Call, ReadTunableValues_Address, &HookedReadTunableValues, "Init" },
		{ StartLoading_ReadTunableValues_Call, ReadTunableValues_Address, &HookedReadTunableValues, "StartLoadingForZoomAndRotation" },
		{ ProcessMessages_ProcessLoadedCells_Call, ProcessLoadedCells_Address, &HookedProcessLoadedCells, "ProcessBackgroundThreadToMainThreadCommunication" },
		{ ProcessLoadedCells_GetElapsedTime_Call, cRZTimer_GetElapsedTime_Address, &HookedProcessLoadedCellsElapsedTime, "ProcessLoadedCells" },
		{ ThreadFunction_GetElapsedTime_Call, cRZTimer_GetElapsedTime_Address, &HookedWorkerSliceElapsedTime, "ThreadFunction timer" },
		{ ThreadFunction_Sleep_Call, SleepCurrentThread_Address, &HookedWorkerSleep, "ThreadFunction sleep" },
		{ AddDirtyRectangle_DoFullViewUpdate_Call, DoFullViewUpdate_Address, &HookedDoFullViewUpdateForDirtyArea, "AddDirtyRectangleInternal" },
		{ InsertModel_DoFullViewUpdate_Call, DoFullViewUpdate_Address, &HookedDoFullViewUpdateForModelChanges, "InsertModel" },
		{ RemoveModel_DoFullViewUpdate_Call, DoFullViewUpdate_Address, &HookedDoFullViewUpdateForModelChanges, "RemoveModel" },
		{ ChangeModel_DoFullViewUpdate_Call, DoFullViewUpdate_Address, &HookedDoFullViewUpdateForModelChanges, "ChangeModel" },
		{ ModeModel_DoFullViewUpdate_Call, DoFullViewUpdate_Address, &HookedDoFullViewUpdateForModelChanges, "ModeModel" },
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

	// MOV EAX,dword ptr [ESI + 0x1D4] at the start of the replaced block, and its queue call.
	static constexpr uint8_t kDirtyQueueBlockStart[] = { 0x8B, 0x86, 0xD4, 0x01, 0x00, 0x00 };

	if (std::memcmp(reinterpret_cast<const void*>(Draw_DirtyQueue_Inject), kDirtyQueueBlockStart, sizeof(kDirtyQueueBlockStart)) != 0
		|| !CallSiteTargets(Draw_DirtyQueue_ProcessCall, ProcessDirtyRectangleQueue_Address))
	{
		logger.WriteLineFormatted(
			LogLevel::Error,
			"Failed to install the model streaming patch: unexpected code at 0x%08X (cSC43DRender::Draw).",
			Draw_DirtyQueue_Inject);
		return;
	}

	if (*reinterpret_cast<const uintptr_t*>(ResourceManager_GetPrivateResource_Slot) != GetPrivateResource_Address)
	{
		logger.WriteLineFormatted(
			LogLevel::Error,
			"Failed to install the model streaming patch: unexpected value at 0x%08X (GetPrivateResource).",
			ResourceManager_GetPrivateResource_Slot);
		return;
	}

	try
	{
		// First: the busier loader thread is only safe once GetPrivateResource is locked.
		Patcher::InstallJumpTableHook(ResourceManager_GetPrivateResource_Slot, &HookedGetPrivateResource);

		for (const CallSite& site : callSites)
		{
			Patcher::InstallCallHook(site.address, site.hook);
		}

		Patcher::InstallHook(Draw_DirtyQueue_Inject, &DrawDirtyQueueHook);

		logger.WriteLineFormatted(
			LogLevel::Info,
			"Installed the model streaming patch (load margin %u MB, frame budget %u ms, visible area %.2f, "
			"tiles %u px, loader slices %u ms, rest %u us).",
			settings.loadMarginMB,
			settings.frameBudgetMs,
			settings.visibleProgress,
			settings.tileSize,
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
