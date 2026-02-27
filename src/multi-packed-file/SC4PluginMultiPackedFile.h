/*
 * This file is part of sc4-dbpf-loading, a DLL Plugin for SimCity 4 that
 * optimizes the DBPF loading.
 *
 * Copyright (C) 2024, 2025, 2026 Nicholas Hayes
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

#pragma once
#include "BaseMultiPackedFile.h"

static const uint32_t GZCLSID_SC4PluginMultiPackedFile = 0x9D92571C;

// A cIGZPersistDBSegmentMultiPackedFiles implementation for .SC4* files (.SC4Desc, .SC4Lot, and .SC4Model)
// that are loaded from the specified root folder and any sub folders.
// This class replaces the game's linear search code with a per-TGI lookup.
class SC4PluginMultiPackedFile final : public BaseMultiPackedFile
{
public:
	SC4PluginMultiPackedFile();

protected:
	std::vector<cRZBaseString> GetDBPFFiles(const cIGZString& folderPath) const override;
};
