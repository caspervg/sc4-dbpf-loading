///////////////////////////////////////////////////////////////////////////////
//
// This file is part of sc4-dbpf-loading, a DLL Plugin for SimCity 4 that
// optimizes the DBPF loading.
//
// Copyright (c) 2024, 2025 Nicholas Hayes
//
// This file is licensed under terms of the MIT License.
// See LICENSE.txt for more information.
//
///////////////////////////////////////////////////////////////////////////////

#include "BaseMultiPackedFile.h"
#include "GZStringConvert.h"
#include "PathUtil.h"
#include "PersistResourceKeyList.h"
#include "Logger.h"
#include "SC4DirectoryEnumerator.h"
#include "cGZPersistResourceKey.h"
#include "cIGZCOM.h"
#include "cIGZFrameWork.h"
#include "cIGZDBSegmentPackedFile.h"
#include "cIGZPersistDBRecord.h"
#include "cIGZPersistResourceKeyFilter.h"
#include "cIGZPersistResourceKeyList.h"
#include "cIGZPersistResourceManager.h"
#include "cRZAutoRefCount.h"
#include "cRZCOMDllDirector.h"
#include "GZServPtrs.h"
#include "wil/resource.h"
#include <atomic>
#include <thread>
#include <Windows.h>

namespace
{
	// Reads the first 64 KB of each file into the OS page cache from a thread pool,
	// overlapping the network/cloud downloads before the main thread's sequential
	// COM-based Open() loop begins. On a local SSD the benefit is minor; on OneDrive
	// or a network share it can trigger N parallel downloads instead of N serial ones.
	//
	// The sequential COM loading loop is unchanged — this only warms the cache.
	void PrefetchFiles(const std::vector<cRZBaseString>& files)
	{
		if (files.size() < 2)
		{
			return;
		}

		// Cap at 8 workers to avoid overwhelming a slow network link and to stay
		// well within the 32-bit process's address space budget for thread stacks.
		const size_t hwThreads = static_cast<size_t>(std::thread::hardware_concurrency());
		const size_t workerCount = std::min(
			hwThreads > 1 ? hwThreads - 1 : 1,
			std::min(files.size(), static_cast<size_t>(8)));

		std::atomic<size_t> nextIndex{ 0 };

		auto worker = [&]()
		{
			constexpr DWORD kPrefetchBytes = 64u * 1024u;
			// Stack-allocate the read buffer — 64 KB is within the default 1 MB stack.
			uint8_t buffer[kPrefetchBytes];

			while (true)
			{
				const size_t idx = nextIndex.fetch_add(1, std::memory_order_relaxed);
				if (idx >= files.size())
				{
					break;
				}

				const cRZBaseString& utf8Path = files[idx];
				std::wstring wpath = GZStringConvert::ToUtf16(utf8Path);

				if (PathUtil::MustAddExtendedPathPrefix(wpath))
				{
					wpath = PathUtil::Normalize(PathUtil::AddExtendedPathPrefix(wpath));
				}

				// FILE_FLAG_SEQUENTIAL_SCAN tells the cache manager to prefetch
				// ahead aggressively; we only need the header region anyway.
				HANDLE hFile = CreateFileW(
					wpath.c_str(),
					GENERIC_READ,
					FILE_SHARE_READ,
					nullptr,
					OPEN_EXISTING,
					FILE_FLAG_SEQUENTIAL_SCAN,
					nullptr);

				if (hFile != INVALID_HANDLE_VALUE)
				{
					DWORD bytesRead = 0;
					ReadFile(hFile, buffer, kPrefetchBytes, &bytesRead, nullptr);
					CloseHandle(hFile);
				}
			}
		};

		std::vector<std::thread> workers;
		workers.reserve(workerCount);
		for (size_t i = 0; i < workerCount; ++i)
		{
			workers.emplace_back(worker);
		}
		for (std::thread& t : workers)
		{
			t.join();
		}
	}

	// Logs a one-time warning when the plugins folder lives on a network or
	// remote drive (DRIVE_REMOTE), which includes UNC shares and mapped drives.
	// OneDrive folders stored under the local profile are reported as DRIVE_FIXED
	// by Windows even though individual files may still be cloud-only; those are
	// caught separately via the unavailableFileCount from the directory scan.
	void LogRemoteDriveWarning(const cIGZString& folderPath)
	{
		std::wstring wpath = GZStringConvert::ToUtf16(folderPath);

		wchar_t volumePath[MAX_PATH + 1]{};
		if (GetVolumePathNameW(wpath.c_str(), volumePath, ARRAYSIZE(volumePath)))
		{
			if (GetDriveTypeW(volumePath) == DRIVE_REMOTE)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Info,
					"The plugin folder '%s' is on a network/remote drive. "
					"Each file open is a round-trip; loading may be significantly slower "
					"than from a local drive.",
					folderPath.ToChar());
			}
		}
	}
}

BaseMultiPackedFile::BaseMultiPackedFile(bool enumerateSegmentsLastInFirstOut)
	: segmentID(0),
	  isOpen(false),
	  initialized(false),
	  enumerateSegmentsLastInFirstOut(enumerateSegmentsLastInFirstOut),
	  criticalSection{}
{
	InitializeCriticalSectionEx(&criticalSection, 0, 0);
}

BaseMultiPackedFile::~BaseMultiPackedFile()
{
	DeleteCriticalSection(&criticalSection);
}

bool BaseMultiPackedFile::QueryInterface(uint32_t riid, void** ppvObj)
{
	if (riid == GZIID_cIGZPersistDBSegmentMultiPackedFiles)
	{
		*ppvObj = static_cast<cIGZPersistDBSegmentMultiPackedFiles*>(this);
		AddRef();

		return true;
	}
	else if (riid == GZIID_cIGZPersistDBSegment)
	{
		*ppvObj = static_cast<cIGZPersistDBSegment*>(this);
		AddRef();

		return true;
	}

	return cRZBaseUnknown::QueryInterface(riid, ppvObj);
}

uint32_t BaseMultiPackedFile::AddRef()
{
	return cRZBaseUnknown::AddRef();
}

uint32_t BaseMultiPackedFile::Release()
{
	return cRZBaseUnknown::Release();
}

bool BaseMultiPackedFile::Init()
{
	if (!initialized)
	{
		initialized = true;
	}

	return true;
}

bool BaseMultiPackedFile::Shutdown()
{
	if (initialized)
	{
		initialized = false;
	}

	return true;
}

bool BaseMultiPackedFile::Open(bool openRead, bool openWrite)
{
	bool result = false;

	// cIGZPersistMultiPackedFiles are always read only.
	if (openRead && !openWrite && folderPath.Strlen() > 0)
	{
		try
		{
			// OPT-4: warn early if the folder is on a network/remote drive so the
			// user knows why startup is slow before the file-open loop begins.
			LogRemoteDriveWarning(folderPath);

			DBPFScanResult scanResult = GetDBPFFiles(folderPath);
			const std::vector<cRZBaseString>& files = scanResult.files;

			// OPT-4: warn about cloud-placeholder or offline files that will each
			// stall the loading loop while their data is fetched from the cloud.
			if (scanResult.unavailableFileCount > 0)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Info,
					"%u plugin file(s) in '%s' are not locally available (cloud-only or offline). "
					"Each will trigger a download during loading. To avoid this delay, "
					"configure the folder to 'Always keep on this device' in OneDrive.",
					scanResult.unavailableFileCount,
					folderPath.ToChar());
			}

			if (!files.empty())
			{
				segments.reserve(files.size());

				// OPT-5: pre-size the TGI map to avoid repeated rehashing as entries
				// are inserted. 64 is a conservative average TGI-entries-per-file;
				// over-reserving wastes a little memory, under-reserving causes rehashes.
				tgiMap.reserve(files.size() * 64);

				// OPT-1: warm the OS page cache (and trigger parallel cloud downloads)
				// before the sequential COM-based Open() loop below. The loop order and
				// semantics are completely unchanged — this only pre-populates the cache.
				PrefetchFiles(files);

				cIGZCOM* pCOM = RZGetFramework()->GetCOMObject();
				cRZAutoRefCount<PersistResourceKeyList> keyList(
					new PersistResourceKeyList(),
					cRZAutoRefCount<PersistResourceKeyList>::kAddRef);

				for (const cRZBaseString& path : files)
				{
					if (!SetupGZPersistDBSegment(path, pCOM, keyList))
					{
						Logger::GetInstance().WriteLineFormatted(
							LogLevel::Error,
							"Failed to load: %s",
							path.ToChar());
					}
				}

				isOpen = segments.size() > 0;
				result = isOpen;
			}
		}
		catch (const std::exception& e)
		{
			Logger::GetInstance().WriteLine(LogLevel::Error, e.what());
			result = false;
		}
	}

	return result;
}

bool BaseMultiPackedFile::IsOpen() const
{
	return isOpen;
}

bool BaseMultiPackedFile::Close()
{
	if (isOpen)
	{
		isOpen = false;

		// Release the cIGZPersistDBSegments that we
		// are holding on to.
		for (cIGZPersistDBSegment* segment : segments)
		{
			segment->Close();
			segment->Shutdown();
			segment->Release();
		}

		segments.clear();
		tgiMap.clear();
	}

	return false;
}

bool BaseMultiPackedFile::Flush()
{
	// cIGZPersistMultiPackedFiles are always read only.
	return true;
}

void BaseMultiPackedFile::GetPath(cIGZString& path) const
{
	path.Copy(folderPath);
}

bool BaseMultiPackedFile::SetPath(cIGZString const& path)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	folderPath.Copy(path);

	return true;
}

bool BaseMultiPackedFile::Lock()
{
	EnterCriticalSection(&criticalSection);
	return true;
}

bool BaseMultiPackedFile::Unlock()
{
	LeaveCriticalSection(&criticalSection);
	return true;
}

uint32_t BaseMultiPackedFile::GetSegmentID() const
{
	return segmentID;
}

bool BaseMultiPackedFile::SetSegmentID(uint32_t const& segmentID)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	this->segmentID = segmentID;
	return true;
}

uint32_t BaseMultiPackedFile::GetRecordCount(cIGZPersistResourceKeyFilter* filter)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	uint32_t count = 0;

	if (isOpen)
	{
		if (filter)
		{
			for (const auto& item : tgiMap)
			{
				if (filter->IsKeyIncluded(item.first))
				{
					count++;
				}
			}
		}
		else
		{
			count = static_cast<uint32_t>(tgiMap.size());
		}
	}

	return count;
}

uint32_t BaseMultiPackedFile::GetResourceKeyList(cIGZPersistResourceKeyList* list, cIGZPersistResourceKeyFilter* filter)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	uint32_t totalResourceCount = 0;

	if (isOpen && list)
	{
		if (enumerateSegmentsLastInFirstOut)
		{
			for (auto iter = segments.rbegin(); iter != segments.rend(); iter++)
			{
				totalResourceCount += (*iter)->GetResourceKeyList(list, filter);
			}
		}
		else
		{
			for (cIGZPersistDBSegment* pSegment : segments)
			{
				totalResourceCount += pSegment->GetResourceKeyList(list, filter);
			}
		}
	}

	return totalResourceCount;
}

bool BaseMultiPackedFile::GetResourceKeyList(cIGZPersistResourceKeyList& list)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen)
	{
		if (enumerateSegmentsLastInFirstOut)
		{
			for (auto iter = segments.rbegin(); iter != segments.rend(); iter++)
			{
				(*iter)->GetResourceKeyList(list);
			}
		}
		else
		{
			for (cIGZPersistDBSegment* pSegment : segments)
			{
				pSegment->GetResourceKeyList(list);
			}
		}
		result = true;
	}

	return result;
}

bool BaseMultiPackedFile::TestForRecord(cGZPersistResourceKey const& key)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen)
	{
		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->TestForRecord(key);
		}
	}

	return result;
}

uint32_t BaseMultiPackedFile::GetRecordSize(cGZPersistResourceKey const& key)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	uint32_t result = 0;

	if (isOpen)
	{
		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->GetRecordSize(key);
		}
	}

	return result;
}

bool BaseMultiPackedFile::OpenRecord(cGZPersistResourceKey const& key, cIGZPersistDBRecord** record, cIGZFile::AccessMode accessMode)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen)
	{
		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->OpenRecord(key, record, accessMode);
		}
	}

	return result;
}

bool BaseMultiPackedFile::CreateNewRecord(cGZPersistResourceKey const& key, cIGZPersistDBRecord** unknown2)
{
	return false;
}

bool BaseMultiPackedFile::CloseRecord(cIGZPersistDBRecord* record)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen && record)
	{
		cGZPersistResourceKey key;
		record->GetKey(key);

		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->CloseRecord(record);
		}
	}

	return result;
}

bool BaseMultiPackedFile::CloseRecord(cIGZPersistDBRecord** record)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen && record && *record)
	{
		cGZPersistResourceKey key;
		(*record)->GetKey(key);

		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->CloseRecord(record);
		}
	}

	return result;
}

bool BaseMultiPackedFile::AbortRecord(cIGZPersistDBRecord* record)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen && record)
	{
		cGZPersistResourceKey key;
		record->GetKey(key);

		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->AbortRecord(record);
		}
	}

	return result;
}

bool BaseMultiPackedFile::AbortRecord(cIGZPersistDBRecord** record)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen && record && *record)
	{
		cGZPersistResourceKey key;
		(*record)->GetKey(key);

		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->AbortRecord(record);
		}
	}

	return result;
}

bool BaseMultiPackedFile::DeleteRecord(cGZPersistResourceKey const& key)
{
	return false;
}

uint32_t BaseMultiPackedFile::ReadRecord(cGZPersistResourceKey const& key, void* buffer, uint32_t& recordSize)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	uint32_t result = 0;

	if (isOpen)
	{
		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			result = item->second->ReadRecord(key, buffer, recordSize);
		}
	}

	return result;
}

bool BaseMultiPackedFile::WriteRecord(cGZPersistResourceKey const& key, void* buffer, uint32_t recordSize)
{
	return false;
}

bool BaseMultiPackedFile::Init(uint32_t segmentID, cIGZString const& path, bool unknown2)
{
	if (!initialized)
	{
		initialized = true;

		this->segmentID = segmentID;
		this->folderPath.Copy(path);
	}

	return true;
}

void BaseMultiPackedFile::SetPathFilter(cIGZString const&)
{
}

int32_t BaseMultiPackedFile::ConsolidateDatabaseRecords(cIGZPersistDBSegment* target, cIGZPersistResourceKeyFilter* filter)
{
	int32_t totalCopiedRecords = 0;

	if (enumerateSegmentsLastInFirstOut)
	{
		for (auto iter = segments.rbegin(); iter != segments.rend(); iter++)
		{
			cIGZDBSegmentPackedFile* pPackedFile = nullptr;

			if ((*iter)->QueryInterface(GZIID_cIGZDBSegmentPackedFile, reinterpret_cast<void**>(&pPackedFile)))
			{
				int32_t copiedRecordCount = pPackedFile->CopyDatabaseRecords(target, filter, false, true);
				totalCopiedRecords += copiedRecordCount;

				pPackedFile->Release();
			}
		}
	}
	else
	{

		for (cIGZPersistDBSegment* pSegment : segments)
		{
			cIGZDBSegmentPackedFile* pPackedFile = nullptr;

			if (pSegment->QueryInterface(GZIID_cIGZDBSegmentPackedFile, reinterpret_cast<void**>(&pPackedFile)))
			{
				int32_t copiedRecordCount = pPackedFile->CopyDatabaseRecords(target, filter, false, true);
				totalCopiedRecords += copiedRecordCount;

				pPackedFile->Release();
			}
		}
	}

	return totalCopiedRecords;
}

int32_t BaseMultiPackedFile::ConsolidateDatabaseRecords(cIGZString const& targetPath, cIGZPersistResourceKeyFilter* filter)
{
	int result = -1;

	cIGZCOM* const pCOM = RZGetFramework()->GetCOMObject();

	cRZAutoRefCount<cIGZPersistDBSegment> pSegment;

	if (pCOM->GetClassObject(
		GZCLSID_cGZDBSegmentPackedFile,
		GZIID_cIGZPersistDBSegment,
		pSegment.AsPPVoid()))
	{
		if (pSegment->Init())
		{
			if (pSegment->SetPath(targetPath))
			{
				if (pSegment->Open(true, true))
				{
					result = ConsolidateDatabaseRecords(pSegment, filter);
					pSegment->Close();
				}
			}

			pSegment->Shutdown();
		}
	}

	return result;
}

bool BaseMultiPackedFile::FindDBSegment(cGZPersistResourceKey const& key, cIGZPersistDBSegment** outSegment)
{
	auto lock = wil::EnterCriticalSection(&criticalSection);

	bool result = false;

	if (isOpen)
	{
		auto item = tgiMap.find(key);

		if (item != tgiMap.end())
		{
			cIGZPersistDBSegment* segment = item->second;

			*outSegment = segment;

			segment->AddRef();
			result = true;
		}
	}

	return result;
}

uint32_t BaseMultiPackedFile::GetSegmentCount()
{
	return static_cast<uint32_t>(segments.size());
}

cIGZPersistDBSegment* BaseMultiPackedFile::GetSegmentByIndex(uint32_t index)
{
	return segments[index];
}

void BaseMultiPackedFile::AddedResource(cGZPersistResourceKey const& key, cIGZPersistDBSegment* pSegment)
{
	if (pSegment)
	{
		tgiMap.insert_or_assign(key, pSegment);
	}
}

void BaseMultiPackedFile::RemovedResource(cGZPersistResourceKey const& key, cIGZPersistDBSegment*)
{
	tgiMap.erase(key);
}

bool BaseMultiPackedFile::SetupGZPersistDBSegment(
	cIGZString const& path,
	cIGZCOM* const pCOM,
	PersistResourceKeyList* const pKeyList)
{
	bool result = false;

	cRZAutoRefCount<cIGZPersistDBSegment> pSegment;

	if (pCOM->GetClassObject(
		GZCLSID_cGZDBSegmentPackedFile,
		GZIID_cIGZPersistDBSegment,
		pSegment.AsPPVoid()))
	{
		if (pSegment->Init())
		{
			if (pSegment->SetPath(path))
			{
				if (pSegment->Open(true, false))
				{
					pSegment->AddRef();

					segments.push_back(pSegment);

					pKeyList->EraseAll();
					pSegment->GetResourceKeyList(pKeyList, nullptr);

					const PersistResourceKeyList::container& keys = pKeyList->GetKeys();
					for (const cGZPersistResourceKey& key : keys)
					{
						tgiMap.insert_or_assign(key, pSegment);
					}
					result = true;
				}
			}
		}
	}

	return result;
}

