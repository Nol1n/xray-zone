#pragma once
#include "step_manager_defs.h"

class CEntityAlive;
class CBlend;
struct SGameMtlPair;

class CStepManager
{
	u8 m_legs_count;

	STEPS_MAP m_steps_map;
	SStepInfo m_step_info;

	CEntityAlive* m_object;

	u16 m_foot_bones[MAX_LEGS_COUNT];
	CBlend* m_blend;
	MotionID m_cpu_step_motion;
	float m_cpu_step_animation_time;
	float m_cpu_step_last_time;
	bool m_cpu_step_clock_initialized;

	struct material_sound
	{
		u8 m_last_step_sound_played;
		SGameMtlPair* last_mtl_pair;

		material_sound(): m_last_step_sound_played(u8(-1)), last_mtl_pair(0)
		{
		}

		void play_next(SGameMtlPair* mtl_pair, CEntityAlive* object, float volume, bool b_hud_mode);
	} m_step_sound;

	u32 m_time_anim_started;

public:
	CStepManager();
	virtual ~CStepManager();

	// init on construction
	virtual DLL_Pure* _construct();
	virtual void reload(LPCSTR section);

	// call on set animation
	void on_animation_start(MotionID motion_id, CBlend* blend);
	// call on updateCL
	void update(bool b_hud_view);
	// Drive gameplay footstep callbacks from a CPU locomotion state on headless objects.
	void update_cpu_footsteps(float dt_seconds);

	// process event
	virtual void event_on_step()
	{
	}

protected:
	Fvector get_foot_position(ELegType leg_type);
	virtual bool is_on_ground() { return true; }
private:
	void reload_foot_bones();
	void load_foot_bones(CInifile::Sect& data);

	float get_blend_time();
};
