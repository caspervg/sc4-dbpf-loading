#include "QFSDecoderHooks.h"

#include "Logger.h"
#include "QFSDecoder.h"

#define NOMINMAX
#include <Windows.h>
#include <cstring>
#include <exception>
#include "detours/detours.h"

namespace
{
	static constexpr uintptr_t DecodeRefAddress = 0x00A6DA3F;

	using DecodeRef = uint32_t(__cdecl*)(
		const uint8_t* input,
		int* consumed,
		uint8_t* output);

	static DecodeRef RealDecodeRef = reinterpret_cast<DecodeRef>(DecodeRefAddress);
}

void QFSDecoderHooks::Install()
{
	Logger& logger = Logger::GetInstance();

	try
	{
		const uint8_t* target = reinterpret_cast<const uint8_t*>(DecodeRefAddress);
		static constexpr uint8_t expectedPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };

		if (std::memcmp(target, expectedPrologue, sizeof(expectedPrologue)) != 0)
		{
			logger.WriteLine(LogLevel::Error, "QFS decoder prologue mismatch; hook was not installed.");
			return;
		}

		DetourRestoreAfterWith();
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		const DecodeRef decoder = QFSDecoder::GetDecoder();
		DetourAttach(
			reinterpret_cast<PVOID*>(&RealDecodeRef),
			reinterpret_cast<PVOID>(decoder));

		const LONG error = DetourTransactionCommit();
		if (error == NO_ERROR)
		{
			logger.WriteLine(
				LogLevel::Info,
				QFSDecoder::UsesAVX2()
					? "Installed the hybrid AVX2 QFS decoder hook."
					: "Installed the scalar QFS decoder hook.");
		}
		else
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"Failed to install the SIMD QFS decoder hook, error code=%d",
				error);
		}
	}
	catch (const std::exception& e)
	{
		logger.WriteLineFormatted(
			LogLevel::Error,
			"Failed to install the SIMD QFS decoder hook: %s",
			e.what());
	}
}
