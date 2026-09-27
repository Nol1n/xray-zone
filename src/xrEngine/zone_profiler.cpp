#include "stdafx.h"
#include "zone_profiler.h"
#include "IGame_Level.h"

#include "../xrCore/FS_impl.h"

#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

extern ENGINE_API BOOL g_appLoaded;
extern bool IsMainMenuActive();

namespace
{
using Clock = std::chrono::steady_clock;
const size_t ZoneCount = static_cast<size_t>(zone_profiler::Zone::Count);
const size_t GaugeCount = static_cast<size_t>(zone_profiler::Gauge::Count);

struct ZoneAggregate
{
	std::atomic<unsigned long long> totalNanoseconds{0};
	std::atomic<unsigned long long> maximumNanoseconds{0};
	std::atomic<unsigned long long> sampleCount{0};
};

std::array<ZoneAggregate, ZoneCount> aggregates;
struct GaugeAggregate
{
	std::atomic<unsigned long long> value{0};
	std::atomic<bool> updated{false};
};

std::array<GaugeAggregate, GaugeCount> gauges;
std::atomic<unsigned long long> luaGcSteps{0};
std::atomic<unsigned long long> luaGcCycles{0};
FILE* csvFile = nullptr;
FILE* metricsFile = nullptr;
bool outputPathReported = false;
bool metricsPathReported = false;

struct FrameRow
{
	unsigned long long sample;
	double elapsedMilliseconds;
	double intervalMilliseconds;
	double idleMilliseconds;
	bool gameplay;
	bool renderActive;
	u32 deviceFrame;
	bool workerTrace;
	bool workerAvailable;
	bool workerJoinAvailable;
	double workerGcMilliseconds;
	unsigned long workerGcSteps;
	double workerJoinMilliseconds;
};

// Only the owning on_idle thread accesses this bounded buffer.
std::array<FrameRow, 2048> frameRows;
size_t frameRowCount = 0;
FILE* framesFile = nullptr;
bool frameOutputFailed = false;
unsigned long long frameSample = 0;
Clock::time_point firstFrameStart;
Clock::time_point previousFrameStart;
Clock::time_point lastFrameOutput;
bool previousGameplayFrame = false;
unsigned long long activeCaptureNanoseconds = 0;
bool captureExitRequested = false;
std::atomic<unsigned long> workerFrame{0};
std::atomic<unsigned long long> workerGcNanoseconds{0};
std::atomic<unsigned long> workerGcSteps{0};
unsigned long mainJoinFrame = 0; // Main on_idle lane only.
unsigned long long mainJoinNanoseconds = 0;

bool flushFrameRows();

struct CsvFileCloser
{
	~CsvFileCloser()
	{
		if (csvFile)
			fclose(csvFile);
		if (metricsFile)
			fclose(metricsFile);
		if (framesFile)
		{
			flushFrameRows();
			fclose(framesFile);
		}
	}
} csvFileCloser;

bool hasArgument(const char* flag)
{
	if (!Core.Params)
		return false;
	const size_t length = strlen(flag);
	for (const char* argument = strstr(Core.Params, flag); argument; argument = strstr(argument + length, flag))
	{
		if ((argument == Core.Params || argument[-1] == ' ' || argument[-1] == '\t') &&
			(argument[length] == 0 || argument[length] == ' ' || argument[length] == '\t'))
			return true;
	}
	return false;
}

bool isEnabled()
{
	static const bool enabled = hasArgument("-zone_profile");
	return enabled;
}

bool frameCaptureEnabled()
{
	static const bool enabled = hasArgument("-zone_frame_capture");
	return enabled;
}

bool isWorkerTraceEnabled()
{
	static const bool enabled = isEnabled() && frameCaptureEnabled() && hasArgument("-zone_worker_trace");
	return enabled;
}

bool captureExitEnabled()
{
	static const bool enabled = frameCaptureEnabled() && hasArgument("-zone_capture_exit");
	return enabled;
}

void requestCaptureExit(const char* reason)
{
	if (!captureExitEnabled() || captureExitRequested)
		return;
	captureExitRequested = true;
	Msg("* [zone-frames] requested diagnostic shutdown: %s", reason);
	FlushLog();
	Engine.Event.Defer("KERNEL:disconnect");
	Engine.Event.Defer("KERNEL:quit");
}

bool openFramesCsv()
{
	string_path path;
	string_path filename;
	snprintf(filename, sizeof(filename), "engine-frames-%lu.csv", static_cast<unsigned long>(GetCurrentProcessId()));
	FS.update_path(path, "$logs$", filename);
	int descriptor = -1;
	const errno_t error = _sopen_s(&descriptor, path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_TEXT,
		_SH_DENYWR, _S_IREAD | _S_IWRITE);
	if (error == 0)
	{
		framesFile = _fdopen(descriptor, "w");
		if (!framesFile)
			_close(descriptor);
	}
	if (!framesFile)
	{
		frameOutputFailed = true;
		Msg("! [zone-frames] could not create CSV (existing files are preserved): %s", path);
		requestCaptureExit("capture output unavailable");
		return false;
	}
	fprintf(framesFile, "sample,elapsed_ms,interval_ms,idle_ms,gameplay,render_active,zone_profile,device_frame,worker_trace,worker_available,worker_gc_ms,worker_gc_steps,worker_join_ms\n");
	fflush(framesFile);
	Msg("* [zone-frames] writing individual frames to %s; zone_profile=%d", path, isEnabled() ? 1 : 0);
	return true;
}

bool flushFrameRows()
{
	if (!framesFile)
		return false;
	for (size_t index = 0; index < frameRowCount; ++index)
	{
		const FrameRow& row = frameRows[index];
		fprintf(framesFile, "%llu,%.6f,%.6f,%.6f,%d,%d,%d,%lu,%d,%d,", row.sample, row.elapsedMilliseconds,
			row.intervalMilliseconds, row.idleMilliseconds, row.gameplay ? 1 : 0,
			row.renderActive ? 1 : 0, isEnabled() ? 1 : 0, static_cast<unsigned long>(row.deviceFrame),
			row.workerTrace ? 1 : 0, row.workerAvailable ? 1 : 0);
		if (row.workerAvailable)
			fprintf(framesFile, "%.6f,%lu,", row.workerGcMilliseconds, row.workerGcSteps);
		else
			fprintf(framesFile, ",,");
		if (row.workerJoinAvailable)
			fprintf(framesFile, "%.6f\n", row.workerJoinMilliseconds);
		else
			fprintf(framesFile, "\n");
	}
	frameRowCount = 0;
	return fflush(framesFile) == 0 && !ferror(framesFile);
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
	case zone_profiler::Zone::AI: return "AI";
	case zone_profiler::Zone::Pathfinding: return "Pathfinding";
	case zone_profiler::Zone::Scheduler: return "Scheduler";
	case zone_profiler::Zone::SchedulerRealtime: return "SchedulerRealtime";
	case zone_profiler::Zone::SchedulerNormal: return "SchedulerNormal";
	case zone_profiler::Zone::SchedulerRegistration: return "SchedulerRegistration";
	case zone_profiler::Zone::WorkerParallel: return "WorkerParallel";
	case zone_profiler::Zone::WorkerFrameMT: return "WorkerFrameMT";
	case zone_profiler::Zone::WorkerGcBatch: return "WorkerGcBatch";
	case zone_profiler::Zone::WorkerJoinWait: return "WorkerJoinWait";
	case zone_profiler::Zone::LuaCoroutine: return "LuaCoroutine";
	case zone_profiler::Zone::LuaCallback: return "LuaCallback";
	case zone_profiler::Zone::ServerTick: return "ServerTick";
	case zone_profiler::Zone::NetworkSerialize: return "NetworkSerialize";
	case zone_profiler::Zone::SavePrepare: return "SavePrepare";
	case zone_profiler::Zone::SavePersistence: return "SavePersistence";
	case zone_profiler::Zone::SaveSerialize: return "SaveSerialize";
	case zone_profiler::Zone::SaveCompress: return "SaveCompress";
	case zone_profiler::Zone::SaveWrite: return "SaveWrite";
	case zone_profiler::Zone::LoadPersistence: return "LoadPersistence";
	case zone_profiler::Zone::LoadRead: return "LoadRead";
	case zone_profiler::Zone::LoadDecompress: return "LoadDecompress";
	case zone_profiler::Zone::LoadDeserialize: return "LoadDeserialize";
	case zone_profiler::Zone::ClientSaveSerialize: return "ClientSaveSerialize";
	case zone_profiler::Zone::LuaGC: return "LuaGC";
	case zone_profiler::Zone::MemorySample: return "MemorySample";
	case zone_profiler::Zone::ProfilerReport: return "ProfilerReport";
	default: return "Unknown";
	}
}

const char* gaugeName(zone_profiler::Gauge gauge)
{
	switch (gauge)
	{
	case zone_profiler::Gauge::LevelObjects: return "LevelObjects";
	case zone_profiler::Gauge::ServerEntities: return "ServerEntities";
	case zone_profiler::Gauge::ALifeRegistryObjects: return "ALifeRegistryObjects";
	case zone_profiler::Gauge::LuaMemoryBytes: return "LuaMemoryBytes";
	case zone_profiler::Gauge::LuaGcStepsTotal: return "LuaGcStepsTotal";
	case zone_profiler::Gauge::LuaGcCyclesTotal: return "LuaGcCyclesTotal";
	case zone_profiler::Gauge::EngineAllocCallsTotal: return "EngineAllocCallsTotal";
	case zone_profiler::Gauge::EngineReallocCallsTotal: return "EngineReallocCallsTotal";
	case zone_profiler::Gauge::EngineFreeCallsTotal: return "EngineFreeCallsTotal";
	case zone_profiler::Gauge::EngineAllocRequestedBytesTotal: return "EngineAllocRequestedBytesTotal";
	case zone_profiler::Gauge::EngineReallocRequestedBytesTotal: return "EngineReallocRequestedBytesTotal";
	case zone_profiler::Gauge::SharedStringEntries: return "SharedStringEntries";
	case zone_profiler::Gauge::SharedStringStorageBytes: return "SharedStringStorageBytes";
	default: return "Unknown";
	}
}

const char* gaugeUnit(zone_profiler::Gauge gauge)
{
	switch (gauge)
	{
	case zone_profiler::Gauge::LuaMemoryBytes:
	case zone_profiler::Gauge::EngineAllocRequestedBytesTotal:
	case zone_profiler::Gauge::EngineReallocRequestedBytesTotal:
	case zone_profiler::Gauge::SharedStringStorageBytes: return "bytes";
	default: return "count";
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
	// Readers may inspect a live capture; no other writer may modify it.
	csvFile = _fsopen(path, "w", _SH_DENYWR);
	if (!csvFile)
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

bool openMetricsCsv()
{
	if (metricsFile)
		return true;

	string_path path;
	string_path filename;
	snprintf(filename, sizeof(filename), "engine-metrics-%lu.csv", static_cast<unsigned long>(GetCurrentProcessId()));
	FS.update_path(path, "$logs$", filename);
	metricsFile = _fsopen(path, "w", _SH_DENYWR);
	if (!metricsFile)
	{
		if (!metricsPathReported)
		{
			Msg("* [zone-profile] could not open metrics CSV: %s", path);
			metricsPathReported = true;
		}
		return false;
	}

	fprintf(metricsFile, "elapsed_ms,metric,value,unit\n");
	fflush(metricsFile);
	Msg("* [zone-profile] writing counters to %s", path);
	return true;
}

void flushGauges(unsigned long long elapsedMilliseconds)
{
	if (!openMetricsCsv())
		return;

	for (size_t index = 0; index < GaugeCount; ++index)
	{
		GaugeAggregate& gauge = gauges[index];
		// A level that has stopped reporting must not produce stale counters.
		if (!gauge.updated.exchange(false, std::memory_order_acquire))
			continue;
		const unsigned long long value = gauge.value.load(std::memory_order_relaxed);
		const char* name = gaugeName(static_cast<zone_profiler::Gauge>(index));
		const char* unit = gaugeUnit(static_cast<zone_profiler::Gauge>(index));
		fprintf(metricsFile, "%llu,%s,%llu,%s\n", elapsedMilliseconds, name, value, unit);
		// Detailed memory telemetry stays in the CSV to avoid extra log I/O.
		if (index <= static_cast<size_t>(zone_profiler::Gauge::ALifeRegistryObjects))
			Msg("* [zone-profile] %s: %llu %s", name, value, unit);
	}
	fflush(metricsFile);
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
	const Clock::time_point reportStart = Clock::now();
	flushSamples(static_cast<unsigned long long>(elapsed.count()));
	flushGauges(static_cast<unsigned long long>(elapsed.count()));
	// Report this cost in the next window; do not recurse into reporting.
	record(zone_profiler::Zone::ProfilerReport, static_cast<unsigned long long>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - reportStart).count()));
}
}

namespace zone_profiler
{
FrameCaptureScope::FrameCaptureScope()
{
	if (!frameCaptureEnabled() || frameOutputFailed || captureExitRequested)
		return;
	if (!framesFile && !openFramesCsv())
		return;
	m_active = true;
	m_start = Clock::now();
	if (!frameSample)
	{
		firstFrameStart = m_start;
		lastFrameOutput = m_start;
	}
	else
		m_intervalNanoseconds = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(m_start - previousFrameStart).count());
	previousFrameStart = m_start;
	m_renderActive = Device.b_is_Active != FALSE;
	m_gameplay = m_renderActive && g_pGameLevel && g_pGameLevel->bReady && g_appLoaded &&
		g_loading_events.empty() && !Device.dwPrecacheFrame && !Device.Paused() && !IsMainMenuActive();
}

FrameCaptureScope::~FrameCaptureScope()
{
	if (!m_active)
		return;
	const Clock::time_point finished = Clock::now();
	const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(m_start - firstFrameStart).count();
	const double idleMilliseconds = std::chrono::duration<double, std::milli>(finished - m_start).count();
	const bool trace = isWorkerTraceEnabled();
	const bool workerAvailable = trace && m_gameplay &&
		workerFrame.load(std::memory_order_acquire) == Device.dwFrame;
	const bool joinAvailable = trace && m_gameplay && mainJoinFrame == Device.dwFrame;
	const double workerGcMs = workerAvailable ?
		static_cast<double>(workerGcNanoseconds.load(std::memory_order_relaxed)) / 1000000.0 : 0.0;
	const unsigned long steps = workerAvailable ? workerGcSteps.load(std::memory_order_relaxed) : 0;
	const double joinMs = joinAvailable ?
		static_cast<double>(mainJoinNanoseconds) / 1000000.0 : 0.0;
	frameRows[frameRowCount++] = { ++frameSample, elapsedMilliseconds,
		static_cast<double>(m_intervalNanoseconds) / 1000000.0, idleMilliseconds, m_gameplay,
		m_renderActive, Device.dwFrame, trace, workerAvailable, joinAvailable, workerGcMs, steps, joinMs };
	if (m_gameplay && previousGameplayFrame)
		activeCaptureNanoseconds += m_intervalNanoseconds;
	previousGameplayFrame = m_gameplay;
	if (frameRowCount == frameRows.size() || finished - lastFrameOutput >= std::chrono::seconds(1))
	{
		lastFrameOutput = finished;
		if (!flushFrameRows())
		{
			frameOutputFailed = true;
			Msg("! [zone-frames] CSV write failed");
			requestCaptureExit("capture output write failed");
		}
	}
	if (captureExitEnabled() && activeCaptureNanoseconds >= 90000000000ULL)
	{
		flushFrameRows();
		requestCaptureExit("90 seconds of active gameplay captured");
	}
}

void setGauge(Gauge gauge, unsigned long long value)
{
	if (!isEnabled())
		return;
	const size_t index = static_cast<size_t>(gauge);
	if (index >= GaugeCount)
		return;
	// Only scalar copies are shared with the reporting thread; no container
	// traversal or engine object lifetime crosses this boundary.
	gauges[index].value.store(value, std::memory_order_relaxed);
	gauges[index].updated.store(true, std::memory_order_release);
}

bool memoryEnabled()
{
	return isEnabled() && xrAllocationTrackingEnabled;
}

bool memorySampleDue()
{
	if (!memoryEnabled())
		return false;
	static Clock::time_point previousSample;
	const Clock::time_point now = Clock::now();
	if (now - previousSample < std::chrono::milliseconds(250))
		return false;
	previousSample = now;
	return true;
}

void recordLuaGcStep(bool cycleCompleted)
{
	if (!memoryEnabled())
		return;
	luaGcSteps.fetch_add(1, std::memory_order_relaxed);
	if (cycleCompleted)
		luaGcCycles.fetch_add(1, std::memory_order_release);
}

bool workerTraceEnabled()
{
	return isWorkerTraceEnabled();
}

void recordWorkerGcBatch(unsigned long frame, unsigned long long nanoseconds, unsigned long steps)
{
	if (!isWorkerTraceEnabled())
		return;
	workerGcNanoseconds.store(nanoseconds, std::memory_order_relaxed);
	workerGcSteps.store(steps, std::memory_order_relaxed);
	workerFrame.store(frame, std::memory_order_release);
}

void recordWorkerJoinWait(unsigned long frame, unsigned long long nanoseconds)
{
	if (!isWorkerTraceEnabled())
		return;
	mainJoinNanoseconds = nanoseconds;
	mainJoinFrame = frame;
}

void collectMemoryGauges(unsigned long long luaMemoryBytes)
{
	if (!memoryEnabled())
		return;
	setGauge(Gauge::LuaMemoryBytes, luaMemoryBytes);
	const unsigned long long cycles = luaGcCycles.load(std::memory_order_acquire);
	setGauge(Gauge::LuaGcStepsTotal, luaGcSteps.load(std::memory_order_relaxed));
	setGauge(Gauge::LuaGcCyclesTotal, cycles);
	const xrAllocationStatistics allocation = xr_get_allocation_statistics();
	setGauge(Gauge::EngineAllocCallsTotal, allocation.allocationCalls);
	setGauge(Gauge::EngineReallocCallsTotal, allocation.reallocationCalls);
	setGauge(Gauge::EngineFreeCallsTotal, allocation.freeCalls);
	setGauge(Gauge::EngineAllocRequestedBytesTotal, allocation.allocationRequestedBytes);
	setGauge(Gauge::EngineReallocRequestedBytesTotal, allocation.reallocationRequestedBytes);
	if (g_pStringContainer)
	{
		u64 entries = 0, storageBytes = 0;
		g_pStringContainer->statistics(entries, storageBytes);
		setGauge(Gauge::SharedStringEntries, entries);
		setGauge(Gauge::SharedStringStorageBytes, storageBytes);
	}
}

Scope::Scope(Zone zone, bool capture) : m_zone(zone), m_start(), m_active(capture && isEnabled())
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
