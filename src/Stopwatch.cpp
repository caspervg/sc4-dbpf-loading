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

#include "Stopwatch.h"
#include <Windows.h>

// This code is based on the .NET runtime Stopwatch and TimeSpan types.

namespace
{
	constexpr int64_t MillisecondsPerSecond = 1000;
	constexpr int64_t SecondsPerMinute = 60;
	constexpr int64_t MinutesPerHour = 60;

	constexpr int64_t TicksPerMillisecond = 10000;
	constexpr int64_t TicksPerSecond = TicksPerMillisecond * MillisecondsPerSecond;
	constexpr int64_t TicksPerMinute = TicksPerSecond * SecondsPerMinute;
	constexpr int64_t TicksPerHour = TicksPerMinute * MinutesPerHour;

	int64_t GetTimeStamp()
	{
		LARGE_INTEGER li{};

		QueryPerformanceCounter(&li);

		return li.QuadPart;
	}

	int64_t GetTickFrequency()
	{
		LARGE_INTEGER li{};

		QueryPerformanceFrequency(&li);

		return TicksPerSecond / li.QuadPart;
	}
}

Stopwatch::Stopwatch() noexcept
	: tickFrequency(GetTickFrequency()), elapsed(0), startTimeStamp(0), isRunning(false)
{
}

int64_t Stopwatch::ElapsedMilliseconds() const
{
	return (GetElapsedTicks() / TicksPerMillisecond);
}

int64_t Stopwatch::ElapsedSeconds() const
{
	return (GetElapsedTicks() / TicksPerSecond);
}

int64_t Stopwatch::ElapsedMinutes() const
{
	return (GetElapsedTicks() / TicksPerMinute);
}

bool Stopwatch::IsRunning() const
{
	return isRunning;
}

void Stopwatch::Reset()
{
	isRunning = false;
	elapsed = 0;
	startTimeStamp = 0;
}

void Stopwatch::Restart()
{
	Reset();
	Start();
}

void Stopwatch::Start()
{
	if (!isRunning)
	{
		startTimeStamp = GetTimeStamp();
		isRunning = true;
	}
}

void Stopwatch::Stop()
{
	if (isRunning)
	{
		int64_t endTimeStamp = GetTimeStamp();
		int64_t elapsedThisPeriod = endTimeStamp - startTimeStamp;
		elapsed += elapsedThisPeriod;
		isRunning = false;
	}
}

int64_t Stopwatch::GetElapsedTicks() const
{
	int64_t timeElapsed = elapsed;

	if (isRunning)
	{
		int64_t currentTimeStamp = GetTimeStamp();
		int64_t elapsedThisPeriod = currentTimeStamp - startTimeStamp;
		timeElapsed += elapsedThisPeriod;
	}

	return timeElapsed * tickFrequency;
}
