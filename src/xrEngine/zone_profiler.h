#pragma once

#include <chrono>

namespace zone_profiler
{
enum class Zone
{
	Frame,
	GameFrame,
	LevelFrame,
	ObjectUpdate,
	NetworkTick,
	PhysicsStep,
	ALifeUpdate,
	AI,
	Pathfinding,
	Scheduler,
	SchedulerRealtime,
	SchedulerNormal,
	SchedulerRegistration,
	WorkerParallel,
	WorkerFrameMT,
	WorkerGcBatch,
	WorkerJoinWait,
	LuaCoroutine,
	LuaCallback,
	ServerTick,
	NetworkSerialize,
	SavePrepare,
	SavePersistence,
	SaveSerialize,
	SaveCompress,
	SaveWrite,
	LoadPersistence,
	LoadRead,
	LoadDecompress,
	LoadDeserialize,
	ClientSaveSerialize,
	LuaGC,
	MemorySample,
	ProfilerReport,
	Count
};

enum class Gauge
{
	LevelObjects,
	ServerEntities,
	ALifeRegistryObjects,
	LuaMemoryBytes,
	LuaGcStepsTotal,
	LuaGcCyclesTotal,
	EngineAllocCallsTotal,
	EngineReallocCallsTotal,
	EngineFreeCallsTotal,
	EngineAllocRequestedBytesTotal,
	EngineReallocRequestedBytesTotal,
	SharedStringEntries,
	SharedStringStorageBytes,
	Count
};

// Record on the owning subsystem's thread. CSV output contains the latest
// observed value in each reporting window, in a separate file from timings.
void setGauge(Gauge gauge, unsigned long long value);

bool memoryEnabled();
// Only called by the level update owner, before render workers resume.
bool memorySampleDue();
void collectMemoryGauges(unsigned long long luaMemoryBytes);
// Called on the existing GC execution lane; no extra Lua work is introduced.
void recordLuaGcStep(bool cycleCompleted);

// One completed worker batch per device frame. The main lane reads it only
// after joining the worker; an absent batch is reported as missing, not zero.
bool workerTraceEnabled();
void recordWorkerGcBatch(unsigned long frame, unsigned long long nanoseconds, unsigned long steps);
void recordWorkerJoinWait(unsigned long frame, unsigned long long nanoseconds);

// Independent, opt-in observation of on_idle calls. Start-to-start intervals
// include reporting overhead; scope time includes waits, not just CPU work.
class FrameCaptureScope
{
public:
	FrameCaptureScope();
	~FrameCaptureScope();
	FrameCaptureScope(const FrameCaptureScope&) = delete;
	FrameCaptureScope& operator=(const FrameCaptureScope&) = delete;

private:
	std::chrono::steady_clock::time_point m_start;
	unsigned long long m_intervalNanoseconds = 0;
	bool m_active = false;
	bool m_gameplay = false;
	bool m_renderActive = false;
};

class Scope
{
public:
	explicit Scope(Zone zone, bool capture = true);
	~Scope();

	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Zone m_zone;
	std::chrono::steady_clock::time_point m_start;
	bool m_active;
};
}
