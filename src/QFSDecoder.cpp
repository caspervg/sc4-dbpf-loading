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

/*
 * Baseline QFS/RefPack decoder and modern-CPU dispatch.
 *
 * Keep this translation unit free of AVX instructions. The AVX2 decoder is
 * compiled separately so feature detection remains safe on baseline x86 PCs.
 */
#include "QFSDecoder.h"

#include <intrin.h>

namespace QFSDecoder::Detail
{
	uint32_t DecodeAVX2(const uint8_t* input, int* consumed, uint8_t* output) noexcept;
}

namespace
{
	using QFSDecoder::DecodeRefFunction;

	uint32_t DecodeScalar(
		const uint8_t* input,
		int* consumed,
		uint8_t* output) noexcept
	{
		if (!input)
		{
			if (consumed) *consumed = 0;
			return 0;
		}

		const uint8_t* source = input + 2;
		if (*input & 1) source = input + 5;

		const uint32_t outputSize =
			(uint32_t(source[0]) << 16) |
			(uint32_t(source[1]) << 8) |
			uint32_t(source[2]);
		source += 3;
		uint8_t* destination = output;

		for (;;)
		{
			const uint8_t code = *source;
			const uint8_t* next = source + 1;

			if (!(code & 0x80))
			{
				const uint8_t b2 = source[1];
				const uint32_t literals = code & 3;
				for (uint32_t i = 0; i != literals; ++i) *destination++ = source[i + 2];
				source += literals + 2;
				uint8_t* back = destination - 1 - (uint32_t(b2) + (code & 0x60) * 8);
				uint8_t* end = destination + ((code & 0x1c) >> 2) + 3;
				while (destination != end) *destination++ = *back++;
				continue;
			}

			if (!(code & 0x40))
			{
				const uint8_t b2 = source[1];
				const uint8_t b3 = source[2];
				const uint32_t literals = b2 >> 6;
				for (uint32_t i = 0; i != literals; ++i) *destination++ = source[i + 3];
				source += literals + 3;
				uint8_t* back = destination - 1 - ((uint32_t(b2 & 0x3f) << 8) + b3);
				uint8_t* end = destination + (code & 0x3f) + 4;
				while (destination != end) *destination++ = *back++;
				continue;
			}

			if (!(code & 0x20))
			{
				const uint8_t b2 = source[1];
				const uint8_t b3 = source[2];
				const uint8_t b4 = source[3];
				const uint32_t literals = code & 3;
				for (uint32_t i = 0; i != literals; ++i) *destination++ = source[i + 4];
				source += literals + 4;
				uint8_t* back = destination - 1 -
					(((uint32_t(code & 0x10) >> 4) << 16) + (uint32_t(b2) << 8) + b3);
				uint8_t* end = destination + ((code & 0x0c) * 0x40) + 5 + b4;
				while (destination != end) *destination++ = *back++;
				continue;
			}

			const uint32_t literalCount = (code & 0x1f) * 4 + 4;
			if (literalCount > 0x70)
			{
				for (uint32_t i = 0; i != (code & 3); ++i) *destination++ = *next++;
				source = next;
				break;
			}

			for (uint32_t i = 0; i != literalCount; ++i) *destination++ = next[i];
			source = next + literalCount;
		}

		if (consumed) *consumed = static_cast<int>(source - input);
		return outputSize;
	}

	bool HasAVX2() noexcept
	{
		int cpuInfo[4] = {};
		__cpuidex(cpuInfo, 0, 0);
		if (cpuInfo[0] < 7) return false;

		__cpuidex(cpuInfo, 1, 0);
		// MSVC may emit FMA instructions for translation units compiled with
		// /arch:AVX2, so include FMA in the required feature set.
		constexpr int FMA = 1 << 12;
		constexpr int OSXSAVE = 1 << 27;
		constexpr int AVX = 1 << 28;
		constexpr int RequiredLeaf1Features = FMA | OSXSAVE | AVX;
		if ((cpuInfo[2] & RequiredLeaf1Features) != RequiredLeaf1Features) return false;

		// XCR0 bits 1 and 2 show that the OS preserves XMM and YMM state.
		constexpr uint64_t XMMAndYMMState = (1ULL << 1) | (1ULL << 2);
		if ((_xgetbv(0) & XMMAndYMMState) != XMMAndYMMState) return false;

		__cpuidex(cpuInfo, 7, 0);
		constexpr int AVX2 = 1 << 5;
		return (cpuInfo[1] & AVX2) != 0;
	}

	struct DecoderDispatch
	{
		DecodeRefFunction function = nullptr;
		bool usesAVX2 = false;
	};

	const DecoderDispatch& GetDispatch() noexcept
	{
		static const DecoderDispatch dispatch = HasAVX2()
			? DecoderDispatch{QFSDecoder::Detail::DecodeAVX2, true}
			: DecoderDispatch{DecodeScalar, false};
		return dispatch;
	}
}

namespace QFSDecoder
{
	DecodeRefFunction GetDecoder() noexcept
	{
		return GetDispatch().function;
	}

	bool UsesAVX2() noexcept
	{
		return GetDispatch().usesAVX2;
	}

	uint32_t Decode(const uint8_t* input, int* consumed, uint8_t* output) noexcept
	{
		return GetDecoder()(input, consumed, output);
	}
}
