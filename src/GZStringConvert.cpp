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

#include "GZStringConvert.h"
#include <stdexcept>
#include <Windows.h>

cRZBaseString GZStringConvert::FromUtf16(const std::wstring& str)
{
	cRZBaseString result;

	const wchar_t* const utf16Chars = str.data();
	const int utf16Length = static_cast<int>(str.length());

	if (utf16Length > 0)
	{
		const int utf8Length = WideCharToMultiByte(
			CP_UTF8,
			0,
			utf16Chars,
			utf16Length,
			nullptr,
			0,
			nullptr,
			nullptr);

		THROW_LAST_ERROR_IF(utf8Length == 0);

		result.Resize(static_cast<uint32_t>(utf8Length));

		const int convertResult = WideCharToMultiByte(
			CP_UTF8,
			0,
			utf16Chars,
			utf16Length,
			result.Data(),
			utf8Length,
			nullptr,
			nullptr);

		THROW_LAST_ERROR_IF(convertResult == 0);
	}

	return result;
}

std::wstring GZStringConvert::ToUtf16(const cIGZString& str)
{
	std::wstring result;

	const char* const utf8Chars = str.Data();
	const int utf8Length = static_cast<int>(str.Strlen());

	if (utf8Length > 0)
	{
		const int utf16Length = MultiByteToWideChar(
			CP_UTF8,
			0,
			utf8Chars,
			utf8Length,
			nullptr,
			0);

		THROW_LAST_ERROR_IF(utf16Length == 0);

		result.resize(static_cast<size_t>(utf16Length), L'\0');

		const int convertResult = MultiByteToWideChar(
			CP_UTF8,
			0,
			utf8Chars,
			utf8Length,
			result.data(),
			utf16Length);

		THROW_LAST_ERROR_IF(convertResult == 0);
	}

	return result;
}
