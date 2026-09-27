////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager.cpp
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_animation_data_storage.h"
#include "stalker_animation_data.h"
#include "stalker_movement_manager_smart_cover.h"
#include "../xrEngine/object_collision_pose.h"
#include "../xrEngine/SkeletonMotions.h"

#include "CharacterPhysicsSupport.h"

// TODO:
// stalker animation manager consists of 5 independent managers,
// they should be represented with the different classes:
//    * head
//    * torso
//    * legs
//    * globals
//    * script

CStalkerAnimationManager::CStalkerAnimationManager(CAI_Stalker* object) :
	m_data_storage(nullptr),
	m_global(object),
	m_head(object),
	m_torso(object),
	m_legs(object),
	m_script(object),
	m_object(object),
	m_visual(nullptr),
	m_skeleton_animated(nullptr),
	m_cpu_root_blend_duration(0.f),
	m_cpu_root_motion_group(0),
	m_cpu_root_motion_active(false),
	m_cpu_locomotion_stopped(false),
	m_cpu_torso_stopped(false),
	m_cpu_head_stopped(false),
	m_cpu_global_stopped(false),
	m_weapon(nullptr),
	m_missile(nullptr),
	m_call_script_callback(false),
	m_call_global_callback(false),
	m_start_new_script_animation(false)
{
	m_cpu_root_motion_animation.invalidate();
	m_cpu_root_start_transform.set(Fidentity);
	m_cpu_root_blend_from.set(Fidentity);
	m_cpu_root_blend_to.set(Fidentity);
}

void CStalkerAnimationManager::reinit()
{
	stop_cpu_root_motion(object().CollisionPose());
	m_cpu_locomotion_animation.invalidate();
	m_cpu_torso_animation.invalidate();
	m_cpu_head_animation.invalidate();
	m_cpu_global_animation.invalidate();
	m_cpu_script_animation.invalidate();
	m_cpu_root_motion_animation.invalidate();
	m_cpu_root_blend_duration = 0.f;
	m_cpu_root_motion_group = 0;
	m_cpu_locomotion_stopped = false;
	m_cpu_torso_stopped = false;
	m_cpu_head_stopped = false;
	m_cpu_global_stopped = false;

	m_direction_start = 0;
	m_current_direction = eMovementDirectionForward;
	m_target_direction = eMovementDirectionForward;

	m_change_direction_time = 0;
	m_looking_back = 0;

	m_no_move_actual = false;

	m_script_animations.clear();

	m_global.reset();
	m_head.reset();
	m_torso.reset();
	m_legs.reset();
	m_script.reset();

	m_legs.step_dependence(true);
	m_global.step_dependence(true);
	m_script.step_dependence(true);

	m_global.global_animation(true);
	m_script.global_animation(true);

	m_call_script_callback = false;

	m_previous_speed = 0.f;
	m_target_speed = 0.f;
	m_last_non_zero_speed = m_target_speed;

	m_special_danger_move = false;
}

void CStalkerAnimationManager::reload()
{
	stop_cpu_root_motion(object().CollisionPose());
	m_visual = object().Visual();
	m_cpu_locomotion_animation.invalidate();
	m_cpu_torso_animation.invalidate();
	m_cpu_head_animation.invalidate();
	m_cpu_global_animation.invalidate();
	m_cpu_script_animation.invalidate();
	m_cpu_root_motion_animation.invalidate();
	m_cpu_root_blend_duration = 0.f;
	m_cpu_root_motion_group = 0;
	m_cpu_locomotion_stopped = false;
	m_cpu_torso_stopped = false;
	m_cpu_head_stopped = false;
	m_cpu_global_stopped = false;

	m_crouch_state_config = object().SpecificCharacter().crouch_type();
	VERIFY((m_crouch_state_config == 0) || (m_crouch_state_config == 1) || (m_crouch_state_config == -1));
	m_crouch_state = m_crouch_state_config;

	if (object().already_dead())
		return;

	if (!m_visual)
	{
		m_skeleton_animated = nullptr;
		IObjectCollisionPose* pose = object().CollisionPose();
		m_data_storage = pose ? stalker_animation_data_storage().object(pose, object().cNameVisual()) : nullptr;
	#ifdef USE_HEAD_BONE_PART_FAKE
		m_script_bone_part_mask = CStalkerAnimationPair::all_bone_parts;
		if (pose && m_data_storage && !m_data_storage->m_head_animations.A.empty())
		{
			u16 head_partition = 0;
			if (pose->motion_partition(m_data_storage->m_head_animations.A.front(), head_partition))
				m_script_bone_part_mask &= ~(1u << head_partition);
		}
	#endif // USE_HEAD_BONE_PART_FAKE
		if (pose)
			pose->set_motion_playback_states(nullptr, 0);
		m_global.reset();
		m_head.reset();
		m_torso.reset();
		m_legs.reset();
		m_script.reset();
		return;
	}

	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_visual);
	VERIFY(m_skeleton_animated);

	m_data_storage = stalker_animation_data_storage().object(m_skeleton_animated);
	VERIFY(m_data_storage);

	if (!object().g_Alive())
		return;

#ifdef USE_HEAD_BONE_PART_FAKE
	VERIFY(!m_data_storage->m_head_animations.A.empty());
	u16 bone_part = m_skeleton_animated->LL_GetMotionDef(m_data_storage->m_head_animations.A.front())->bone_or_part;
	VERIFY(bone_part != BI_NONE);
	m_script_bone_part_mask = CStalkerAnimationPair::all_bone_parts ^ (1 << bone_part);
#endif

	assign_bone_callbacks();

#ifdef DEBUG
	global().set_dbg_info		(*object().cName(),"Global");
	head().set_dbg_info			(*object().cName(),"Head  ");
	torso().set_dbg_info		(*object().cName(),"Torso ");
	legs().set_dbg_info			(*object().cName(),"Legs  ");
	script().set_dbg_info		(*object().cName(),"Script");
#endif

	m_global.reset();
	m_head.reset();
	m_torso.reset();
	m_legs.reset();
	m_script.reset();
};

void CStalkerAnimationManager::stop_cpu_root_motion(IObjectCollisionPose* pose)
{
	if (!m_cpu_root_motion_active)
	{
		m_cpu_root_motion_animation.invalidate();
		m_cpu_root_blend_duration = 0.f;
		m_cpu_root_motion_group = 0;
		return;
	}

	if (pose)
		pose->set_root_motion_extraction(false);

	CCharacterPhysicsSupport* physics = object().character_physics_support();
	if (physics)
	{
		if (physics->movement())
			physics->movement()->SetPosition(object().Position());
		physics->on_destroy_anim_mov_ctrl();
	}

	m_cpu_root_motion_active = false;
	m_cpu_root_motion_animation.invalidate();
	m_cpu_root_blend_duration = 0.f;
	m_cpu_root_motion_group = 0;
}

bool CStalkerAnimationManager::update_cpu_root_motion(IObjectCollisionPose* pose, const MotionID& motion,
	                                                   const Fmatrix* requested_start_transform,
	                                                   const SMotionPlaybackState* state, u8 controller_group)
{
	if (!pose || !motion.valid() || !state)
	{
		stop_cpu_root_motion(pose);
		return false;
	}

	const bool new_motion = !m_cpu_root_motion_active || m_cpu_root_motion_animation != motion ||
		m_cpu_root_motion_group != controller_group;
	Fmatrix blend_target = Fidentity;
	float blend_duration = 0.f;
	Fmatrix start_pose;
	if (new_motion)
	{
		if (requested_start_transform)
			start_pose.set(*requested_start_transform);
		else
			start_pose.set(object().XFORM());
		blend_duration = 0.2f * state->time_total;
		if (blend_duration > EPS_S)
		{
			SMotionPlaybackState blend_state = *state;
			blend_state.time_current = _min(blend_duration, state->time_total);
			Fmatrix root_transform;
			if (!pose->sample_root_motion_transform(blend_state, root_transform))
			{
				stop_cpu_root_motion(pose);
				return false;
			}
			blend_target.mul_43(start_pose, root_transform);
		}
		else
		{
			Fmatrix root_transform;
			if (!pose->sample_root_motion_transform(*state, root_transform))
			{
				stop_cpu_root_motion(pose);
				return false;
			}
			blend_target.mul_43(start_pose, root_transform);
		}
		if (!_valid(start_pose) || !_valid(blend_target))
		{
			stop_cpu_root_motion(pose);
			return false;
		}

		if (!m_cpu_root_motion_active)
		{
			CCharacterPhysicsSupport* physics = object().character_physics_support();
			if (!physics || !physics->movement() || object().animation_movement_controlled() ||
				!pose->set_root_motion_extraction(true))
				return false;
			physics->on_create_anim_mov_ctrl();
			m_cpu_root_motion_active = true;
		}

		m_cpu_root_start_transform.set(start_pose);
		m_cpu_root_blend_from.set(object().XFORM());
		m_cpu_root_blend_to.set(blend_target);
		m_cpu_root_blend_duration = blend_duration;
		m_cpu_root_motion_animation = motion;
		m_cpu_root_motion_group = controller_group;
	}

	Fmatrix world_transform;
	if (m_cpu_root_blend_duration > EPS_S && state->time_current < m_cpu_root_blend_duration)
	{
		const float factor = _min(1.f, state->time_current / m_cpu_root_blend_duration);
		Fquaternion from_rotation = Fquaternion().set(m_cpu_root_blend_from);
		Fquaternion to_rotation = Fquaternion().set(m_cpu_root_blend_to);
		Fquaternion blended_rotation;
		blended_rotation.slerp(from_rotation, to_rotation, factor);
		world_transform.rotation(blended_rotation);
		world_transform.c.lerp(m_cpu_root_blend_from.c, m_cpu_root_blend_to.c, factor);
	}
	else
	{
		Fmatrix root_transform;
		if (!pose->sample_root_motion_transform(*state, root_transform))
		{
			stop_cpu_root_motion(pose);
			return false;
		}
		world_transform.mul_43(m_cpu_root_start_transform, root_transform);
	}

	if (!_valid(world_transform))
	{
		stop_cpu_root_motion(pose);
		return false;
	}

	object().XFORM().set(world_transform);
	object().spatial_move();
	return true;
}

bool CStalkerAnimationManager::update_cpu_animations()
{
	if (m_visual || !m_data_storage)
		return false;

	IObjectCollisionPose* pose = object().CollisionPose();
	if (!pose)
		return false;
	const int state_capacity = MAX_CHANNELS * MAX_BLENDED;
	SMotionPlaybackState states[MAX_CHANNELS * MAX_BLENDED];
	int state_count = 0;
	if (!pose->get_motion_playback_states(states, state_capacity, state_count))
		return false;

	if (!object().g_Alive())
	{
		stop_cpu_root_motion(pose);
		bool changed = false;
		for (int i = 0; i < state_count; ++i)
		{
			if (states[i].controller_group <= 4 &&
				(states[i].playing || states[i].blend_state != eMotionPlaybackBlendFixed || states[i].callback_enabled))
			{
				states[i].playing = false;
				states[i].blend_state = eMotionPlaybackBlendFixed;
				states[i].callback_enabled = false;
				changed = true;
			}
		}
		if (changed && !pose->set_motion_playback_states(states, state_count))
			return false;
		m_cpu_locomotion_stopped = true;
		m_cpu_torso_stopped = true;
		m_cpu_head_stopped = true;
		m_cpu_global_stopped = true;
		return changed;
	}

	auto motion_finished = [&](const MotionID& id, u8 controller_group) {
		if (!id.valid())
			return false;
		for (int i = 0; i < state_count; ++i)
			if (states[i].id == id && states[i].controller_group == controller_group && states[i].callback_enabled &&
				states[i].blend_state != eMotionPlaybackBlendFalloff && states[i].stop_at_end && !states[i].playing)
				return true;
		return false;
	};
	if (!m_cpu_locomotion_stopped && motion_finished(m_cpu_locomotion_animation, 0))
	{
		m_cpu_locomotion_stopped = true;
		legs().on_animation_end();
	}
	if (!m_cpu_torso_stopped && motion_finished(m_cpu_torso_animation, 1))
	{
		m_cpu_torso_stopped = true;
		on_cpu_torso_animation_end();
	}
	if (!m_cpu_head_stopped && motion_finished(m_cpu_head_animation, 2))
	{
		m_cpu_head_stopped = true;
		head().on_animation_end();
	}
	if (m_cpu_script_animation.valid() && motion_finished(m_cpu_script_animation, 4))
	{
		const MotionID completed_script = m_cpu_script_animation;
		m_cpu_script_animation.invalidate();
		if (m_cpu_root_motion_group == 4 && m_cpu_root_motion_animation == completed_script)
			m_cpu_root_motion_animation.invalidate();
		if (!m_script_animations.empty() && m_script_animations.front().animation() == completed_script)
			pop_script_animation();
		m_call_script_callback = true;
		m_start_new_script_animation = true;
		script().on_animation_end();
	}
	if (!m_cpu_global_stopped && motion_finished(m_cpu_global_animation, 3))
	{
		if (m_cpu_root_motion_group == 3 && m_cpu_root_motion_animation == m_cpu_global_animation)
			m_cpu_root_motion_animation.invalidate();
		m_cpu_global_stopped = true;
		global().on_animation_end();
		if (m_global_callback)
			m_call_global_callback = true;
	}
	play_delayed_callbacks();

	MotionID selected_script;
	const CStalkerAnimationScript* selected_script_request = nullptr;
	bool selected_script_root_motion = false;
	if (!m_script_animations.empty())
	{
		const CStalkerAnimationScript& request = m_script_animations.front();
		selected_script = request.animation();
		selected_script_request = &request;
		selected_script_root_motion = request.use_movement_controller() || request.has_transform() ||
			pose->motion_uses_root_mover(request.animation());
	}
	if (selected_script.valid())
		script().animation(selected_script);

	bool animation_movement_controller = false;
	MotionID selected_global;
	if (!selected_script.valid())
		selected_global = assign_global_animation(animation_movement_controller);
	if (selected_global.valid())
		global().animation(selected_global);
	MotionID selected_root_motion;
	const Fmatrix* selected_root_start_transform = nullptr;
	u8 selected_root_motion_group = 0;
	if (selected_script_root_motion && selected_script_request)
	{
		selected_root_motion = selected_script;
		selected_root_motion_group = 4;
		if (selected_script_request->has_transform())
			selected_root_start_transform = &selected_script_request->transform(object());
	}
	else if (selected_global.valid() &&
		(animation_movement_controller || pose->motion_uses_root_mover(selected_global)))
	{
		selected_root_motion = selected_global;
		selected_root_motion_group = 3;
		selected_root_start_transform = global().target_matrix_ptr();
	}

	const MotionID selected_legs = (selected_global.valid() || selected_script.valid()) ? MotionID() : assign_legs_animation();
	const MotionID selected_torso = (selected_global.valid() || selected_script.valid()) ? MotionID() : assign_torso_animation();
	const MotionID selected_head = assign_head_animation_cpu();
	if (selected_torso.valid())
		torso().animation(selected_torso);
	if (selected_head.valid())
		head().animation(selected_head);
	MotionID next_legs = m_cpu_locomotion_animation;
	MotionID next_torso = m_cpu_torso_animation;
	MotionID next_head = m_cpu_head_animation;
	MotionID next_global = m_cpu_global_animation;
	MotionID next_script = m_cpu_script_animation;
	bool next_script_stopped = false;
	bool next_legs_stopped = m_cpu_locomotion_stopped;
	bool next_torso_stopped = m_cpu_torso_stopped;
	bool next_head_stopped = m_cpu_head_stopped;
	bool next_global_stopped = m_cpu_global_stopped;
	bool changed = false;

	auto update_partition = [&](MotionID& current, bool& stopped, const MotionID& selected, u8 controller_group,
							 u32 partition_mask) {
		bool current_state_found = false;
		bool current_state_active = false;
		for (int i = 0; i < state_count; ++i)
		{
			if (states[i].controller_group != controller_group)
				continue;
			if (states[i].id == current)
			{
				current_state_found = true;
				current_state_active = current_state_active ||
					states[i].blend_state != eMotionPlaybackBlendFalloff;
			}
		}

		if (selected == current && (!selected.valid() || (current_state_found && current_state_active && !stopped)))
			return true;

		if (!selected.valid())
		{
			bool needs_falloff = current.valid();
			for (int i = 0; i < state_count; ++i)
			{
				SMotionPlaybackState& state = states[i];
				if (state.controller_group != controller_group)
					continue;
				if (state.blend_state != eMotionPlaybackBlendFalloff || state.callback_enabled)
				{
					state.blend_state = eMotionPlaybackBlendFalloff;
					state.callback_enabled = false;
					needs_falloff = true;
				}
			}
			if (!needs_falloff)
				return true;
			current.invalidate();
			stopped = false;
			changed = true;
			return true;
		}

		SMotionPlaybackState new_state;
		new_state.id = selected;
		new_state.channel = 0;
		new_state.controller_group = controller_group;
		new_state.partition_mask = partition_mask;
		if (!pose->configure_motion_playback_state(new_state))
			return false;
		new_state.weight = EPS_S;
		new_state.blend_state = eMotionPlaybackBlendAccrue;

		if (controller_group == 0 && current_state_found && legs().step_dependence() && !fis_zero(m_target_speed))
		{
			for (int i = 0; i < state_count; ++i)
			{
				if (states[i].controller_group != controller_group || states[i].id != current ||
					states[i].time_total <= EPS_S)
					continue;
				const float cycle_phase = states[i].time_current / states[i].time_total;
				new_state.time_current = cycle_phase * new_state.time_total;
				break;
			}
		}

		for (int i = 0; i < state_count; ++i)
		{
			SMotionPlaybackState& state = states[i];
			if (state.controller_group != controller_group)
				continue;
			state.blend_state = eMotionPlaybackBlendFalloff;
			state.blend_falloff = new_state.blend_falloff;
			state.callback_enabled = false;
		}

		int channel_state_count = 0;
		for (int i = 0; i < state_count; ++i)
			channel_state_count += states[i].channel == new_state.channel;
		while (channel_state_count >= MAX_BLENDED || state_count >= state_capacity)
		{
			int lightest_fade = -1;
			float smallest_weight = 2.f;
			for (int i = 0; i < state_count; ++i)
			{
				if (states[i].channel != new_state.channel || states[i].blend_state != eMotionPlaybackBlendFalloff)
					continue;
				if (states[i].weight < smallest_weight)
				{
					lightest_fade = i;
					smallest_weight = states[i].weight;
				}
			}
			if (lightest_fade < 0)
				return false;
			for (int i = lightest_fade + 1; i < state_count; ++i)
				states[i - 1] = states[i];
			--state_count;
			--channel_state_count;
		}

		states[state_count++] = new_state;
		current = selected;
		stopped = false;
		changed = true;
		return true;
	};

	if (selected_script.valid())
	{
		if (!update_partition(next_legs, next_legs_stopped, MotionID(), 0, 0) ||
			!update_partition(next_torso, next_torso_stopped, MotionID(), 1, 0) ||
			!update_partition(next_head, next_head_stopped, selected_head, 2, 0) ||
			!update_partition(next_global, next_global_stopped, MotionID(), 3, 0) ||
			!update_partition(next_script, next_script_stopped, selected_script, 4, m_script_bone_part_mask))
			return false;
	}
	else if (selected_global.valid())
	{
		if (!update_partition(next_legs, next_legs_stopped, MotionID(), 0, 0) ||
			!update_partition(next_torso, next_torso_stopped, MotionID(), 1, 0) ||
			!update_partition(next_head, next_head_stopped, MotionID(), 2, 0) ||
			!update_partition(next_global, next_global_stopped, selected_global, 3,
				CStalkerAnimationPair::all_bone_parts) ||
			!update_partition(next_script, next_script_stopped, MotionID(), 4, 0))
			return false;
	}
	else if (!update_partition(next_global, next_global_stopped, MotionID(), 3, 0) ||
		!update_partition(next_legs, next_legs_stopped, selected_legs, 0, 0) ||
		!update_partition(next_torso, next_torso_stopped, selected_torso, 1, 0) ||
		!update_partition(next_head, next_head_stopped, selected_head, 2, 0) ||
		!update_partition(next_script, next_script_stopped, MotionID(), 4, 0))
		return false;
	if (!changed)
	{
		if (selected_root_motion.valid())
		{
			for (int i = 0; i < state_count; ++i)
				if (states[i].id == selected_root_motion && states[i].controller_group == selected_root_motion_group)
					update_cpu_root_motion(pose, selected_root_motion, selected_root_start_transform, &states[i],
						selected_root_motion_group);
		}
		else if (m_cpu_root_motion_active)
			stop_cpu_root_motion(pose);
		return false;
	}
	if (!pose->set_motion_playback_states(states, state_count))
		return false;

	m_cpu_locomotion_animation = next_legs;
	m_cpu_torso_animation = next_torso;
	m_cpu_head_animation = next_head;
	m_cpu_global_animation = next_global;
	m_cpu_script_animation = next_script;
	m_cpu_locomotion_stopped = next_legs_stopped;
	m_cpu_torso_stopped = next_torso_stopped;
	m_cpu_head_stopped = next_head_stopped;
	m_cpu_global_stopped = next_global_stopped;
	if (selected_root_motion.valid())
	{
		bool applied = false;
		for (int i = 0; i < state_count; ++i)
		{
			if (states[i].id != selected_root_motion || states[i].controller_group != selected_root_motion_group)
				continue;
			applied = update_cpu_root_motion(pose, selected_root_motion, selected_root_start_transform, &states[i],
				selected_root_motion_group);
			break;
		}
		if (!applied && m_cpu_root_motion_active)
			stop_cpu_root_motion(pose);
	}
	else if (m_cpu_root_motion_active)
		stop_cpu_root_motion(pose);
	return true;
}

void CStalkerAnimationManager::play_fx(float power_factor, int fx_index)
{
	VERIFY(fx_index >= 0);
	VERIFY(fx_index < (int)m_data_storage->m_part_animations.A[object().movement().body_state()].m_global.A[0].A.size())
	;
#ifdef DEBUG
	if (psAI_Flags.is(aiAnimation)) {
		LPCSTR					name = m_skeleton_animated->LL_MotionDefName_dbg(m_data_storage->m_part_animations.A[object().movement().body_state()].m_global.A[0].A[fx_index]).first;
		Msg						("%6d [%s][%s][%s][%f]",Device.dwTimeGlobal,*object().cName(),"FX",name,power_factor);
	}
#endif
	m_skeleton_animated->PlayFX(
		m_data_storage->m_part_animations.A[object().movement().body_state()].m_global.A[0].A[fx_index], power_factor);
}

bool CStalkerAnimationManager::standing() const
{
	CAI_Stalker& obj = object();
	stalker_movement_manager_smart_cover& movement = obj.movement();

	if (movement.speed(obj.character_physics_support()->movement()) < EPS_L)
		return (true);

	if (eMovementTypeStand == movement.movement_type())
		return (true);

	return (false);
}
