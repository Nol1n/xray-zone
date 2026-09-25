#include "stdafx.h"
#include "zone_profiler.h"

#include "../xrCore/FS_impl.h"

#include <array>
#include <atomic>
#include <chrono>

namespace
{
using Clock = std::chrono::steady_clock;
const size_t ZoneCount = static_cast<size_t>(zone_profiler::Zone::Count);

struct ZoneAggregate
{
	std::atomic<unsigned long long> totalNanoseconds{0};
	std::atomic<unsigned long long> maximumNanoseconds{0};
	std::atomic<unsigned long long> sampleCount{0};
};

std::array<ZoneAggregate, ZoneCount> aggregates;
FILE* csvFile = nullptr;
bool outputPathReported = false;

struct CsvFileCloser
{
	~CsvFileCloser()
	{
		if (csvFile)
			fclose(csvFile);
	}
} csvFileCloser;

bool isEnabled()
{
	static const bool enabled = Core.Params && strstr(Core.Params, "-zone_profile");
	return enabled;
}

const char* zoneName(zone_profiler::Zone zone)
{
	switch (zone)
	{
	case zone_profiler::Zone::Frame: return "Frame";
	case zone_profiler::Zone::GameFrame: return "GameFrame";
	case zone_profiler::Zone::LevelFrame: return "LevelFrame";
	case zone_profiler::Zone::ObjectUpdate: return "ObjectUpdate";
	case zone_profiler::Zone::NetworkTick: return "NetworkTick";
	case zone_profiler::Zone::PhysicsStep: return "PhysicsStep";
	case zone_profiler::Zone::ALifeUpdate: return "ALifeUpdate";
	default: return "Unknown";
	}
}

void record(zone_profiler::Zone zone, unsigned long long elapsedNanoseconds)
{
	const size_t index = static_cast<size_t>(zone);
	if (index >= ZoneCount)
		return;

	ZoneAggregate& aggregate = aggregates[index];
	aggregate.totalNanoseconds.fetch_add(elapsedNanoseconds, std::memory_order_relaxed);
	aggregate.sampleCount.fetch_add(1, std::memory_order_relaxed);

	unsigned long long previousMaximum = aggregate.maximumNanoseconds.load(std::memory_order_relaxed);
	while (previousMaximum < elapsedNanoseconds &&
		!aggregate.maximumNanoseconds.compare_exchange_weak(previousMaximum, elapsedNanoseconds,
			std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
}

bool openCsv()
{
	if (csvFile)
		return true;

	string_path path;
	string_path filename;
	snprintf(filename, sizeof(filename), "engine-profile-%lu.csv", static_cast<unsigned long>(GetCurrentProcessId()));
	FS.update_path(path, "$logs$", filename);
	if (fopen_s(&csvFile, path, "w") != 0 || !csvFile)
	{
		if (!outputPathReported)
		{
			Msg("* [zone-profile] could not open CSV output: %s", path);
			outputPathReported = true;
		}
		return false;
	}

	fprintf(csvFile, "elapsed_ms,zone,samples,total_ms,average_ms,max_ms\n");
	fflush(csvFile);
	Msg("* [zone-profile] writing timings to %s", path);
	return true;
}

void flushSamples(unsigned long long elapsedMilliseconds)
{
	if (!openCsv())
		return;

	for (size_t index = 0; index < ZoneCount; ++index)
	{
		ZoneAggregate& aggregate = aggregates[index];
		const unsigned long long count = aggregate.sampleCount.exchange(0, std::memory_order_relaxed);
		const unsigned long long totalNanoseconds = aggregate.totalNanoseconds.exchange(0, std::memory_order_relaxed);
		const unsigned long long maximumNanoseconds = aggregate.maximumNanoseconds.exchange(0, std::memory_order_relaxed);
		if (!count)
			continue;

		const double totalMilliseconds = static_cast<double>(totalNanoseconds) / 1000000.0;
		const double averageMilliseconds = totalMilliseconds / static_cast<double>(count);
		const double maximumMilliseconds = static_cast<double>(maximumNanoseconds) / 1000000.0;
		const zone_profiler::Zone zone = static_cast<zone_profiler::Zone>(index);

		fprintf(csvFile, "%llu,%s,%llu,%.6f,%.6f,%.6f\n", elapsedMilliseconds, zoneName(zone), count,
			totalMilliseconds, averageMilliseconds, maximumMilliseconds);
		Msg("* [zone-profile] %s: %llu samples, avg %.3f ms, max %.3f ms",
			zoneName(zone), count, averageMilliseconds, maximumMilliseconds);
	}

	fflush(csvFile);
}

void flushIfDue(Clock::time_point now)
{
	static const Clock::time_point sessionStart = now;
	static Clock::time_point lastFlush = now;
	if (now - lastFlush < std::chrono::seconds(1))
		return;

	lastFlush = now;
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sessionStart);
	flushSamples(static_cast<unsigned long long>(elapsed.count()));
}
}

namespace zone_profiler
{
Scope::Scope(Zone zone) : m_zone(zone), m_start(), m_active(isEnabled())
{
	if (m_active)
		m_start = Clock::now();
}

Scope::~Scope()
{
	if (!m_active)
		return;

	const Clock::time_point finished = Clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - m_start);
	record(m_zone, static_cast<unsigned long long>(elapsed.count()));
	if (m_zone == Zone::Frame)
		flushIfDue(finished);
}
}
