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
	Count
};

class Scope
{
public:
	explicit Scope(Zone zone);
	~Scope();

	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Zone m_zone;
	std::chrono::steady_clock::time_point m_start;
	bool m_active;
};
}
