#include "stdafx.h"
#include "step_manager.h"
#include "entity_alive.h"
#include "../Include/xrRender/Kinematics.h"
#include "xrEngine/SkeletonMotions.h"
#include "xrEngine/object_collision_pose.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "level.h"
#include "gamepersistent.h"
#include "material_manager.h"
#include "profiler.h"
#include "IKLimbsController.h"
#include "GameObject.h"
#include "../../xrServerEntities/script_engine.h"

#ifdef	DEBUG
BOOL debug_step_info = FALSE;
BOOL debug_step_info_load = FALSE;
#endif

extern float psHUDStepSoundVolume;

CStepManager::CStepManager()	: m_legs_count(0), m_object(nullptr), m_blend(nullptr), m_cpu_step_animation_time(0.f),
													  m_cpu_step_last_time(0.f), m_cpu_step_clock_initialized(false), m_time_anim_started(0)
{
}

CStepManager::~CStepManager()
{
}

DLL_Pure* CStepManager::_construct()
{
	m_object = smart_cast<CEntityAlive*>(this);
	VERIFY(m_object);
	return (m_object);
}

void CStepManager::reload(LPCSTR section)
{
	m_legs_count = pSettings->r_u8(section, "LegsCount");
	LPCSTR anim_section = pSettings->r_string(section, "step_params");
	m_steps_map.clear();
	m_step_info.disable = true;
	m_cpu_step_motion.invalidate();
	m_cpu_step_animation_time = 0.f;
	m_cpu_step_last_time = 0.f;
	m_cpu_step_clock_initialized = false;
	m_time_anim_started = 0;
	m_blend = 0;
	for (u32 i = 0; i < MAX_LEGS_COUNT; ++i)
		m_foot_bones[i] = BI_NONE;

	if (!pSettings->section_exist(anim_section))
	{
#ifdef	DEBUG
		Msg( "! no step_params section for :%s section :s", m_object->cName().c_str(), section );
#endif
		return;
	}
	VERIFY((m_legs_count>=MIN_LEGS_COUNT) && (m_legs_count<=MAX_LEGS_COUNT));

	SStepParam param;
	param.step[0].time = 0.1f; // avoid warning

	LPCSTR anim_name, val;
	string16 cur_elem;

	IKinematicsAnimated* skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());
	IObjectCollisionPose* collision_pose = m_object->CollisionPose();
	if (!skeleton_animated && !collision_pose)
	{
#ifdef DEBUG
		Msg("! CStepManager::reload skipped: no CPU pose for object:%s", m_object->cName().c_str());
#endif
		return;
	}
#ifdef	DEBUG
		if( debug_step_info_load )
			Msg( "loading step_params for object :%s, visual: %s, section: %s, step_params section: %s  ", m_object->cName().c_str(), m_object->cNameVisual().c_str(), section, anim_section );
#endif

	for (u32 i = 0; pSettings->r_line(anim_section, i, &anim_name, &val); ++i)
	{
		_GetItem(val, 0, cur_elem);

		param.cycles = u8(atoi(cur_elem));
		R_ASSERT(param.cycles >= 1);

		for (u32 j = 0; j < m_legs_count; j++)
		{
			_GetItem(val, 1 + j * 2, cur_elem);
			param.step[j].time = float(atof(cur_elem));
			_GetItem(val, 1 + j * 2 + 1, cur_elem);
			param.step[j].power = float(atof(cur_elem));
			VERIFY(_valid(param.step[j].power));
		}

		MotionID motion_id;
		if (skeleton_animated)
			motion_id = skeleton_animated->ID_Cycle_Safe(anim_name);
		else if (!collision_pose->find_cycle(anim_name, motion_id))
			motion_id.invalidate();
		if (!motion_id)
		{
#ifdef	DEBUG
		Msg("! (CStepManager::reload) no step motion '%s' for object:%s, visual:%s, section:%s", anim_name,
			m_object->cName().c_str(), m_object->cNameVisual().c_str(), anim_section);

#endif
			continue;
		}
#ifdef	DEBUG
		if( debug_step_info_load )
		{
			IKinematicsAnimated *KA = smart_cast<IKinematicsAnimated*>(m_object->Visual());
			if (KA)
			{
				std::pair<LPCSTR,LPCSTR> loaded_name = KA->LL_MotionDefName_dbg(motion_id);
				Msg("step_params loaded for object:%s, visual:%s, motion:%s, anim set:%s", m_object->cName().c_str(),
					m_object->cNameVisual().c_str(), loaded_name.first, loaded_name.second);
			}
			else
				Msg("step_params loaded for CPU object:%s, motion id:%u/%u", m_object->cName().c_str(), motion_id.slot,
					motion_id.idx);
		}
#endif
		m_steps_map.insert(mk_pair(motion_id, param));
	}

#ifdef	DEBUG
	if( m_steps_map.empty() )
		Msg( "! no steps info loaded for :%s, section :s, step_params section: %s ", m_object->cName().c_str(), section, anim_section );
#endif
	// Foot bones are needed only for client-side particles. The CPU server path
	// emits the gameplay callback from the configured phase and collision state.
	if (skeleton_animated)
		reload_foot_bones();
}

void CStepManager::update_cpu_footsteps(float dt_seconds)
{
	if (!m_object || m_object->Visual() || !std::isfinite(dt_seconds) || dt_seconds <= 0.f)
		return;

	IObjectCollisionPose* pose = m_object->CollisionPose();
	if (!pose)
		return;

	SMotionPlaybackState states[MAX_CHANNELS * MAX_BLENDED];
	int state_count = 0;
	if (!pose->get_motion_playback_states(states, MAX_CHANNELS * MAX_BLENDED, state_count))
		return;

	const SMotionPlaybackState* locomotion = nullptr;
	for (int i = 0; i < state_count; ++i)
	{
		const SMotionPlaybackState& state = states[i];
		if (state.controller_group != 0 || state.blend_state == eMotionPlaybackBlendFalloff ||
			state.weight <= EPS_S || !state.playing)
			continue;
		if (!locomotion || state.weight > locomotion->weight)
			locomotion = &state;
	}
	if (!locomotion)
	{
		m_cpu_step_clock_initialized = false;
		return;
	}

	const STEPS_MAP_IT step = m_steps_map.find(locomotion->id);
	if (step == m_steps_map.end() || step->second.cycles == 0 || locomotion->time_total <= EPS_S ||
		locomotion->speed <= EPS_S)
	{
		m_step_info.disable = true;
		m_cpu_step_motion = locomotion->id;
		m_cpu_step_clock_initialized = false;
		return;
	}

	CCharacterPhysicsSupport* const physics = m_object->character_physics_support();
	const bool on_ground = physics && physics->movement() &&
		physics->movement()->Environment() == CPHMovementControl::peOnGround;
	const bool has_material_pair = m_object->material().get_current_pair() != nullptr;
	CGameObject* const game_object = smart_cast<CGameObject*>(m_object);
	auto emit_footstep = [&](u32 leg) {
		if (on_ground && has_material_pair && game_object)
			game_object->FootStepCallback(m_step_info.params.step[leg].power, false, on_ground, false);
	};

	if (m_cpu_step_motion != locomotion->id || !m_cpu_step_clock_initialized)
	{
		m_cpu_step_motion = locomotion->id;
		m_step_info.params = step->second;
		m_step_info.disable = false;
		m_cpu_step_animation_time = locomotion->time_current;
		m_cpu_step_last_time = locomotion->time_current;
		m_cpu_step_clock_initialized = true;
		if (locomotion->time_current <= EPS_S)
			for (u32 leg = 0; leg < m_legs_count; ++leg)
				if (std::fabs(m_step_info.params.step[leg].time) <= EPS_S)
					emit_footstep(leg);
		return;
	}

	const float animation_delta = dt_seconds * locomotion->speed;
	if (!std::isfinite(animation_delta) || animation_delta <= 0.f)
		return;

	const float total_time = locomotion->time_total;
	float expected_phase = m_cpu_step_last_time + animation_delta;
	if (locomotion->stop_at_end)
		expected_phase = _min(expected_phase, total_time);
	else
	{
		expected_phase = std::fmod(expected_phase, total_time);
		if (expected_phase < 0.f)
			expected_phase += total_time;
	}
	if (std::fabs(expected_phase - locomotion->time_current) > _max(0.02f, animation_delta * 0.1f))
	{
		// A restart/phase correction occurred outside this step clock; resync
		// instead of synthesizing duplicate foot contacts.
		m_cpu_step_animation_time = locomotion->time_current;
		m_cpu_step_last_time = locomotion->time_current;
		if (locomotion->time_current <= EPS_S)
			for (u32 leg = 0; leg < m_legs_count; ++leg)
				if (std::fabs(m_step_info.params.step[leg].time) <= EPS_S)
					emit_footstep(leg);
		return;
	}

	const float interval = total_time / float(m_step_info.params.cycles);
	if (!std::isfinite(interval) || interval <= EPS_S)
		return;

	const float previous_time = m_cpu_step_animation_time;
	const float current_time = previous_time + animation_delta;
	const u64 first_cycle = static_cast<u64>(std::floor(previous_time / interval));
	for (u32 leg = 0; leg < m_legs_count; ++leg)
	{
		const float step_phase = m_step_info.params.step[leg].time;
		if (!std::isfinite(step_phase) || step_phase < 0.f || step_phase > 1.f)
			continue;
		float event_time = (float(first_cycle) + step_phase) * interval;
		if (event_time <= previous_time + EPS_S)
			event_time += interval;
		if (event_time > current_time + EPS_S)
			continue;
		emit_footstep(leg);
	}

	m_cpu_step_animation_time = current_time;
	m_cpu_step_last_time = locomotion->time_current;
}

void CStepManager::on_animation_start(MotionID motion_id, CBlend* blend)
{
	m_blend = blend;
	if (!m_blend) return;

	if (m_object->character_ik_controller())
		m_object->character_ik_controller()->PlayLegs(blend);

	m_time_anim_started = Device.dwTimeGlobal;

	// искать текущую анимацию в STEPS_MAP
	STEPS_MAP_IT it = m_steps_map.find(motion_id);
	if (it == m_steps_map.end())
	{
#ifdef	DEBUG
		if( debug_step_info )
		{
			IKinematicsAnimated *KA = smart_cast<IKinematicsAnimated*>(m_object->Visual());
			VERIFY( KA );
			std::pair<LPCSTR,LPCSTR> anim_name = KA->LL_MotionDefName_dbg( motion_id );
			Msg( "! no step_params found for object :%s, visual: %s, motion: %s, anim set: %s  ", m_object->cName().c_str(), m_object->cNameVisual().c_str(), anim_name.first, anim_name.second );
		}
#endif
		m_step_info.disable = true;
		return;
	}

	m_step_info.disable = false;
	m_step_info.params = it->second;
	m_step_info.cur_cycle = 1; // all cycles are 1-based

	for (u32 i = 0; i < m_legs_count; i++)
	{
		m_step_info.activity[i].handled = false;
		m_step_info.activity[i].cycle = m_step_info.cur_cycle;
	}


	VERIFY(m_blend);
}


void CStepManager::update(bool b_hud_view)
{
	START_PROFILE("Step Manager")
		if (m_step_info.disable) return;
		if (!m_blend) return;

		float dist = m_object->Position().distance_to(Device.vCameraPosition);
		bool b_play = dist < 50.0f; //meters

		// получить параметры шага
		SStepParam& step = m_step_info.params;
		u32 cur_time = Device.dwTimeGlobal;

		// время одного цикла анимации
		float cycle_anim_time = get_blend_time() / step.cycles;

		// пройти по всем ногам и проверить время
		SGameMtlPair* mtl_pair = 0;
		bool material_picked = false;

		for (u32 i = 0; i < m_legs_count; i++)
		{
			// если событие уже обработано для этой ноги, то skip
			if (m_step_info.activity[i].handled && (m_step_info.activity[i].cycle == m_step_info.cur_cycle))
				continue;

			// вычислить смещённое время шага в соответствии с параметрами анимации ходьбы
			u32 offset_time = m_time_anim_started + u32(
				1000 * (cycle_anim_time * (m_step_info.cur_cycle - 1) + cycle_anim_time * step.step[i].time));
			if (offset_time <= cur_time)
			{
				if (!material_picked)
				{
					mtl_pair = m_object->material().get_current_pair();

					material_picked = true;
				}

				if (!mtl_pair)
					break;

				CGameObject* object = smart_cast<CGameObject*>(m_object);
				if (b_play && is_on_ground() && object)
				{
					if (object->ID() == 0)
					{
						SGameMtl* mt = GMLib.GetMaterialByID(mtl_pair->GetMtl1());
						if (mt)
						{
							::luabind::functor<bool>	funct;
							if (ai().script_engine().functor("_G.CActor__FootstepCallback", funct))
							{
								if (funct(*mt->m_Name, m_step_info.params.step[i].power, b_hud_view))
									m_step_sound.play_next(mtl_pair, m_object, m_step_info.params.step[i].power, b_hud_view);
							}
						}
					}
					else
					{
						m_step_sound.play_next(mtl_pair, m_object, m_step_info.params.step[i].power, b_hud_view);
						object->FootStepCallback(m_step_info.params.step[i].power, b_play, is_on_ground(), b_hud_view);
					}	
				}

				// Играть партиклы
				if (b_play && !mtl_pair->CollideParticles.empty())
				{
					LPCSTR ps_name = *mtl_pair->CollideParticles[::Random.randI(0, mtl_pair->CollideParticles.size())];

					//отыграть партиклы столкновения материалов
					CParticlesObject* ps = CParticlesObject::Create(ps_name,TRUE);

					// вычислить позицию и направленность партикла
					Fmatrix pos;

					// установить направление
					pos.k.set(Fvector().set(0.0f, 1.0f, 0.0f));
					Fvector::generate_orthonormal_basis(pos.k, pos.j, pos.i);

					// установить позицию
					pos.c.set(get_foot_position(ELegType(i)));

					ps->UpdateParent(pos, Fvector().set(0.f, 0.f, 0.f));
					GamePersistent().ps_needtoplay.push_back(ps);
				}

				// Play Camera FXs
				event_on_step();

				// обновить поле handle
				m_step_info.activity[i].handled = true;
				m_step_info.activity[i].cycle = m_step_info.cur_cycle;
			}
		}

		// определить текущий цикл
		if (m_step_info.cur_cycle < step.cycles)
			m_step_info.cur_cycle = 1 + u8(float(cur_time - m_time_anim_started) / (1000.f * cycle_anim_time));

		// если анимация циклическая...
		u32 time_anim_end = m_time_anim_started + u32(get_blend_time() * 1000); // время завершения работы анимации
		if (!m_blend->stop_at_end && (time_anim_end < cur_time))
		{
			m_time_anim_started = time_anim_end;
			m_step_info.cur_cycle = 1;

			for (u32 i = 0; i < m_legs_count; i++)
			{
				m_step_info.activity[i].handled = false;
				m_step_info.activity[i].cycle = m_step_info.cur_cycle;
			}
		}
	STOP_PROFILE
}

//////////////////////////////////////////////////////////////////////////
// Function for foot processing
//////////////////////////////////////////////////////////////////////////
Fvector CStepManager::get_foot_position(ELegType leg_type)
{
	R_ASSERT2(m_foot_bones[leg_type] != BI_NONE, "foot bone had not been set");

	IKinematics* pK = smart_cast<IKinematics*>(m_object->Visual());
	const Fmatrix& bone_transform = pK->LL_GetBoneInstance(m_foot_bones[leg_type]).mTransform;

	Fmatrix global_transform;
	global_transform.mul_43(m_object->XFORM(), bone_transform);

	return global_transform.c;
}

void CStepManager::load_foot_bones(CInifile::Sect& data)
{
	for (CInifile::SectCIt I = data.Data.begin(); I != data.Data.end(); ++I)
	{
		const CInifile::Item& item = *I;

		u16 index = smart_cast<IKinematics*>(m_object->Visual())->LL_BoneID(*item.second);
		VERIFY3(index != BI_NONE, "foot bone not found", *item.second);

		if (xr_strcmp(*item.first, "front_left") == 0) m_foot_bones[eFrontLeft] = index;
		else if (xr_strcmp(*item.first, "front_right") == 0) m_foot_bones[eFrontRight] = index;
		else if (xr_strcmp(*item.first, "back_right") == 0) m_foot_bones[eBackRight] = index;
		else if (xr_strcmp(*item.first, "back_left") == 0) m_foot_bones[eBackLeft] = index;
	}
}

void CStepManager::reload_foot_bones()
{
	CInifile* ini = smart_cast<IKinematics*>(m_object->Visual())->LL_UserData();
	if (ini && ini->section_exist("foot_bones"))
	{
		load_foot_bones(ini->r_section("foot_bones"));
	}
	else
	{
		if (!pSettings->line_exist(*m_object->cNameSect(), "foot_bones"))
			R_ASSERT2(false, "section [foot_bones] not found in monster user_data");
		load_foot_bones(pSettings->r_section(pSettings->r_string(*m_object->cNameSect(), "foot_bones")));
	}

	// проверка на соответсвие
	int count = 0;
	for (u32 i = 0; i < MAX_LEGS_COUNT; i++)
		if (m_foot_bones[i] != BI_NONE) count++;

	VERIFY(count == m_legs_count);
}

float CStepManager::get_blend_time()
{
	return (m_blend->timeTotal / m_blend->speed);
}

void CStepManager::material_sound::play_next(SGameMtlPair* mtl_pair, CEntityAlive* object, float volume,
                                             bool b_hud_mode)
{
	if (mtl_pair->StepSounds.empty())
		return;

	Fvector sound_pos = object->Position();
	sound_pos.y += 0.5;

	if (last_mtl_pair != mtl_pair || m_last_step_sound_played == u8(-1))
	{
		m_last_step_sound_played = u8(Random.randI(mtl_pair->StepSounds.size()));
		last_mtl_pair = mtl_pair;
	}
	else
	{
		u8 new_played = u8(
			(m_last_step_sound_played + 1 + Random.randI(mtl_pair->StepSounds.size() - 1)) % mtl_pair
			                                                                                 ->StepSounds.size());

		m_last_step_sound_played = new_played;
	}

	float vol = (b_hud_mode) ? volume * psHUDStepSoundVolume : volume;
	if (b_hud_mode)
		sound_pos.set(0, 0, 0);

	mtl_pair->StepSounds[m_last_step_sound_played].play_no_feedback(object,
	                                                                b_hud_mode ? sm_2D : 0,
	                                                                0,
	                                                                &sound_pos,
	                                                                &vol);
}
