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
		constexpr int OSXSAVE = 1 << 27;
		constexpr int AVX = 1 << 28;
		if ((cpuInfo[2] & (OSXSAVE | AVX)) != (OSXSAVE | AVX)) return false;
		if ((_xgetbv(0) & 0x6) != 0x6) return false;

		__cpuidex(cpuInfo, 7, 0);
		return (cpuInfo[1] & (1 << 5)) != 0;
	}

	struct DecoderDispatch
	{
		DecodeRefFunction function;
		bool usesAVX2;
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
