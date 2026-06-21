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

#pragma once
#include "cRZBaseString.h"
#include <cstdint>
#include <vector>

struct DBPFScanResult
{
	std::vector<cRZBaseString> files;

	// The number of files whose data is not immediately available locally.
	// These are cloud-placeholder or offline files that will trigger a network
	// download (or remote read) when SC4 opens them during the loading loop.
	// A non-zero value means startup will be delayed by download time.
	uint32_t unavailableFileCount = 0;
};
