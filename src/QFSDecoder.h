/*
 * QFS/RefPack decoder used by the SC4 DBPF loading optimization plugin.
 */
#pragma once

#include <cstdint>

namespace QFSDecoder
{
	using DecodeRefFunction = uint32_t(__cdecl*)(
		const uint8_t* input,
		int* consumed,
		uint8_t* output);

	DecodeRefFunction GetDecoder() noexcept;
	bool UsesAVX2() noexcept;
	uint32_t Decode(const uint8_t* input, int* consumed, uint8_t* output) noexcept;
}
