/*
 * QFS/RefPack decoder for SimCity 4.
 *
 * The scalar state machine mirrors cRZFastCompression3::decoderef in the
 * Windows 1.1.641 executable. The AVX2 copy routines accelerate the large
 * literal and long-copy cases while preserving RefPack overlap semantics.
 */
#include "QFSDecoder.h"

#include <cstring>
#include <intrin.h>
#include <immintrin.h>

namespace
{
	using QFSDecoder::DecodeRefFunction;

	static uint32_t DecodeScalar(
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

				for (uint32_t i = 0; i != literals; ++i)
				{
					*destination++ = source[i + 2];
				}

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

				for (uint32_t i = 0; i != literals; ++i)
				{
					*destination++ = source[i + 3];
				}

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

				for (uint32_t i = 0; i != literals; ++i)
				{
					*destination++ = source[i + 4];
				}

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
				for (uint32_t i = 0; i != (code & 3); ++i)
				{
					*destination++ = *next++;
				}

				source = next;
				break;
			}

			for (uint32_t i = 0; i != literalCount; ++i)
			{
				*destination++ = next[i];
			}

			source = next + literalCount;
		}

		if (consumed) *consumed = static_cast<int>(source - input);
		return outputSize;
	}

	__declspec(noinline) void CopyNonOverlapAVX2(
		uint8_t* destination,
		const uint8_t* source,
		size_t count) noexcept
	{
		while (count >= 32)
		{
			const __m256i value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source));
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(destination), value);
			source += 32;
			destination += 32;
			count -= 32;
		}

		if (count >= 16)
		{
			const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(destination), value);
			source += 16;
			destination += 16;
			count -= 16;
		}

		if (count) std::memcpy(destination, source, count);
	}

	__declspec(noinline) void CopyOverlapAVX2(
		uint8_t* destination,
		const uint8_t* source,
		size_t count,
		size_t distance) noexcept
	{
		if (count < 32 || distance < 32)
		{
			size_t seeded = 0;
			while (seeded < count && seeded < distance)
			{
				destination[seeded] = source[seeded];
				++seeded;
			}

			size_t copied = seeded;
			while (copied < count)
			{
				const size_t chunk = (copied < count - copied) ? copied : count - copied;
				std::memcpy(destination + copied, destination, chunk);
				copied += chunk;
			}
			return;
		}

		CopyNonOverlapAVX2(destination, source, count);
	}

	__declspec(noinline) uint32_t DecodeAVX2(
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
			const uint8_t code = *source++;

			if (!(code & 0x80))
			{
				const uint8_t b2 = *source++;
				const size_t literals = code & 3;
				if (literals) { std::memcpy(destination, source, literals); destination += literals; source += literals; }
				const size_t length = ((code & 0x1c) >> 2) + 3;
				const size_t distance = size_t(b2) + (code & 0x60) * 8 + 1;
				CopyOverlapAVX2(destination, destination - distance, length, distance);
				destination += length;
				continue;
			}

			if (!(code & 0x40))
			{
				const uint8_t b2 = *source++;
				const uint8_t b3 = *source++;
				const size_t literals = b2 >> 6;
				if (literals) { std::memcpy(destination, source, literals); destination += literals; source += literals; }
				const size_t length = (code & 0x3f) + 4;
				const size_t distance = (size_t(b2 & 0x3f) << 8) | b3;
				CopyOverlapAVX2(destination, destination - distance - 1, length, distance + 1);
				destination += length;
				continue;
			}

			if (!(code & 0x20))
			{
				const uint8_t b2 = *source++;
				const uint8_t b3 = *source++;
				const uint8_t b4 = *source++;
				const size_t literals = code & 3;
				if (literals) { std::memcpy(destination, source, literals); destination += literals; source += literals; }
				const size_t length = ((code & 0x0c) << 6) + b4 + 5;
				const size_t distance = (((code & 0x10) >> 4) << 16) | (size_t(b2) << 8) | b3;
				CopyOverlapAVX2(destination, destination - distance - 1, length, distance + 1);
				destination += length;
				continue;
			}

			const size_t literals = (code & 0x1f) * 4 + 4;
			if (literals > 0x70)
			{
				const size_t tail = code & 3;
				if (tail) { std::memcpy(destination, source, tail); destination += tail; source += tail; }
				break;
			}

			std::memcpy(destination, source, literals);
			destination += literals;
			source += literals;
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
		if ((cpuInfo[2] & (1 << 27)) == 0 || (cpuInfo[2] & (1 << 28)) == 0) return false;
		__cpuidex(cpuInfo, 7, 0);
		return (cpuInfo[1] & (1 << 5)) != 0;
	}
}

namespace QFSDecoder
{
	uint32_t Decode(const uint8_t* input, int* consumed, uint8_t* output) noexcept
	{
		static const DecodeRefFunction decoder = HasAVX2() ? DecodeAVX2 : DecodeScalar;
		return decoder(input, consumed, output);
	}
}
