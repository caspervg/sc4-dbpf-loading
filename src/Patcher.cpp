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

#include "Patcher.h"
#include <Windows.h>
#include "wil/resource.h"
#include "wil/win32_helpers.h"

void Patcher::OverwriteMemory(uintptr_t address, uint8_t newValue)
{
	DWORD oldProtect;
	// Allow the executable memory to be written to.
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		reinterpret_cast<LPVOID>(address),
		sizeof(newValue),
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	// Patch the memory at the specified address.
	*((uint8_t*)address) = newValue;
}

void Patcher::OverwriteMemory(uintptr_t address, uintptr_t newValue)
{
	DWORD oldProtect;
	// Allow the executable memory to be written to.
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		reinterpret_cast<LPVOID>(address),
		sizeof(newValue),
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	// Patch the memory at the specified address.
	*((uintptr_t*)address) = newValue;
}

void Patcher::InstallHook(uintptr_t targetAddress, void (*pfnFunc)())
{
	// Allow the executable memory to be written to.
	DWORD oldProtect = 0;
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		reinterpret_cast<LPVOID>(targetAddress),
		5,
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	// Patch the memory at the specified address.
	*((uint8_t*)targetAddress) = 0xE9;
	*((uintptr_t*)(targetAddress + 1)) = reinterpret_cast<uintptr_t>(pfnFunc) - targetAddress - 5;
}


void Patcher::InstallCallHook(uintptr_t targetAddress, void* pfnFunc)
{
	// Allow the executable memory to be written to.
	DWORD oldProtect = 0;
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		reinterpret_cast<LPVOID>(targetAddress),
		5,
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	// Patch the memory at the specified address.
	*((uint8_t*)targetAddress) = 0xE8;
	*((uintptr_t*)(targetAddress + 1)) = ((uintptr_t)pfnFunc) - targetAddress - 5;
}

void Patcher::InstallJumpTableHook(uintptr_t targetAddress, void* pfnFunc)
{
	OverwriteMemory(targetAddress, reinterpret_cast<uintptr_t>(pfnFunc));
}
