#include "stdafx.h"
#include "control_animation.h"
#include "BaseMonster/base_monster.h"
#include "control_animation_base.h"
#include "control_manager.h"
#include "profiler.h"
#include "../../../xrEngine/object_collision_pose.h"
#include "../../../xrEngine/SkeletonMotions.h"

//#ifdef _DEBUG
//#include "control_animation_base.h"
//#endif

void SAnimationPart::set_motion(MotionID const& m)
{
	VERIFY(m.valid());
	motion = m;
}

void CControlAnimation::reinit()
{
	inherited::reinit();

	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());
	m_cpu_global_motion.invalidate();
	m_cpu_legs_motion.invalidate();
	m_cpu_torso_motion.invalidate();

	m_anim_events.clear();

	m_global_animation_end = false;
	m_legs_animation_end = false;
	m_torso_animation_end = false;

	m_freeze = false;
}

void CControlAnimation::reset_data()
{
	m_data.global.init();
	m_data.legs.init();
	m_data.torso.init();
	m_data.set_speed(-1.f);
}

void CControlAnimation::update_frame()
{
	if (m_freeze || !m_skeleton_animated) return;

	// move to schedule update
	START_PROFILE("BaseMonster/Animation/Update Tracks")
		;
		m_skeleton_animated->UpdateTracks();
	STOP_PROFILE;

	START_PROFILE("BaseMonster/Animation/Check callbacks")
		;
		check_callbacks();
	STOP_PROFILE;

	START_PROFILE("BaseMonster/Animation/Play")
		;
		play();
	STOP_PROFILE;

	START_PROFILE("BaseMonster/Animation/Check Events")
		;
		check_events(m_data.global);
		check_events(m_data.torso);
		check_events(m_data.legs);
	STOP_PROFILE;
}

void CControlAnimation::update_schedule()
{
	if (!m_object || !m_object->g_Alive())
		return;
	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());
	if (m_skeleton_animated)
		return;

	update_cpu_playback();
}

static void global_animation_end_callback(CBlend* B)
{
	CControlAnimation* controller = (CControlAnimation *)B->CallbackParam;
	controller->m_global_animation_end = true;
}

static void legs_animation_end_callback(CBlend* B)
{
	CControlAnimation* controller = (CControlAnimation *)B->CallbackParam;
	controller->m_legs_animation_end = true;
}

static void torso_animation_end_callback(CBlend* B)
{
	CControlAnimation* controller = (CControlAnimation *)B->CallbackParam;
	controller->m_torso_animation_end = true;
}

void CControlAnimation::play()
{
	if (!m_data.global.actual)
	{
		play_part(m_data.global, global_animation_end_callback);
		if (m_data.global.blend) m_saved_global_speed = m_data.global.blend->speed;
	}

	if (!m_data.legs.actual)
		play_part(m_data.legs, legs_animation_end_callback);
	if (!m_data.torso.actual)
		play_part(m_data.torso, torso_animation_end_callback);

	// speed only for global
	if (m_data.global.blend)
	{
		if (m_data.get_speed() > 0)
		{
			m_data.global.blend->speed = m_data.get_speed(); // TODO: make factor
		}
		else
		{
			m_data.global.blend->speed = m_saved_global_speed;
		}
	}
}

void CControlAnimation::play_part(SAnimationPart& part, PlayCallback callback)
{
	VERIFY(part.get_motion().valid());

	u16 bone_or_part = m_skeleton_animated->LL_GetMotionDef(part.get_motion())->bone_or_part;
	if (bone_or_part == u16(-1)) bone_or_part = m_skeleton_animated->LL_PartID("default");

	// initialize synchronization of prev and current animation
	float pos = -1.f;
	if (part.blend && !part.blend->stop_at_end)
		pos = fmod(part.blend->timeCurrent, part.blend->timeTotal) / part.blend->timeTotal;
#ifdef DEBUG
	//IKinematicsAnimated * K = m_object->Visual()->dcast_PKinematicsAnimated();
	//Msg				("%6d Playing animation : %s , %s , Object %s",Device.dwTimeGlobal, K->LL_MotionDefName_dbg(part.motion).first,K->LL_MotionDefName_dbg(part.motion).second, *(m_object->cName()));
#endif

	part.blend = m_skeleton_animated->LL_PlayCycle(bone_or_part, part.get_motion(), TRUE, callback, this);


	///////////////////////////////////////////////////////////////////////////////
	//#ifdef _DEBUG	
	//	Msg("Monster[%s] Time[%u] Anim[%s]",*(m_object->cName()), Device.dwTimeGlobal,*(m_object->anim().GetAnimTranslation(part.motion)));
	//#endif
	///////////////////////////////////////////////////////////////////////////////

	// synchronize prev and current animations
	if ((pos > 0) && part.blend && !part.blend->stop_at_end)
	{
		part.blend->timeCurrent = part.blend->timeTotal * pos;
	}

	part.time_started = Device.dwTimeGlobal;
	part.actual = true;

	m_man->notify(ControlCom::eventAnimationStart, 0);

	if ((part.get_motion() != m_data.torso.get_motion()) && part.blend)
		m_object->CStepManager::on_animation_start(part.get_motion(), part.blend);


	ANIMATION_EVENT_MAP_IT it = m_anim_events.find(part.get_motion());
	if (it != m_anim_events.end())
	{
		for (ANIMATION_EVENT_VEC_IT event_it = it->second.begin(); event_it != it->second.end(); ++event_it)
		{
			event_it->handled = false;
		}
	}
}


void CControlAnimation::add_anim_event(MotionID motion, float time_perc, u32 id)
{
	// if there is already event with exact timing - return
	ANIMATION_EVENT_MAP_IT it = m_anim_events.find(motion);
	if (it != m_anim_events.end())
	{
		ANIMATION_EVENT_VEC& anim_vec = it->second;

		for (ANIMATION_EVENT_VEC_IT I = anim_vec.begin(); I != anim_vec.end(); ++I)
		{
			if (fsimilar(I->time_perc, time_perc)) return;
		}
	}

	SAnimationEvent event;
	event.time_perc = time_perc;
	event.event_id = id;

	m_anim_events[motion].push_back(event);
}

void CControlAnimation::check_events(SAnimationPart& part)
{
	if (part.get_motion().valid() && part.actual && part.blend)
	{
		ANIMATION_EVENT_MAP_IT it = m_anim_events.find(part.get_motion());
		if (it != m_anim_events.end())
		{
			float cur_perc = float(Device.dwTimeGlobal - part.time_started) / ((part.blend->timeTotal / part
			                                                                                            .blend->speed) *
				1000);

			for (ANIMATION_EVENT_VEC_IT event_it = it->second.begin(); event_it != it->second.end(); ++event_it)
			{
				SAnimationEvent& event = *event_it;
				if (!event.handled && (event.time_perc < cur_perc))
				{
					event.handled = true;

					// gen event
					SAnimationSignalEventData anim_event(part.get_motion(), event.time_perc, event.event_id);
					m_man->notify(ControlCom::eventAnimationSignal, &anim_event);
				}
			}
		}
	}
}

void CControlAnimation::check_callbacks()
{
	if (m_global_animation_end)
	{
		m_man->notify(ControlCom::eventAnimationEnd, 0);
		m_global_animation_end = false;
	}

	if (m_legs_animation_end)
	{
		m_man->notify(ControlCom::eventLegsAnimationEnd, 0);
		m_legs_animation_end = false;
	}

	if (m_torso_animation_end)
	{
		m_man->notify(ControlCom::eventTorsoAnimationEnd, 0);
		m_torso_animation_end = false;
	}
}

float CControlAnimation::current_animation_duration() const
{
	if (m_data.global.blend && m_data.global.blend->speed > EPS_S)
		return m_data.global.blend->timeTotal / m_data.global.blend->speed;

	IObjectCollisionPose* pose = m_object ? m_object->CollisionPose() : nullptr;
	if (!pose || !m_cpu_global_motion.valid())
		return 0.f;

	SMotionPlaybackState states[MAX_CHANNELS * MAX_BLENDED];
	int state_count = 0;
	if (!pose->get_motion_playback_states(states, MAX_CHANNELS * MAX_BLENDED, state_count))
		return 0.f;

	for (int i = 0; i < state_count; ++i)
	{
		const SMotionPlaybackState& state = states[i];
		if (state.controller_group == 3 && state.id == m_cpu_global_motion && state.speed > EPS_S)
			return state.time_total / state.speed;
	}
	return 0.f;
}

bool CControlAnimation::update_cpu_playback()
{
	IObjectCollisionPose* pose = m_object->CollisionPose();
	if (!pose)
		return false;

	check_callbacks();
	CControlAnimationBase& selection = m_object->anim();
	selection.update_frame();
	if (!m_data.global.get_motion().valid())
		selection.select_animation();

	const int state_capacity = MAX_CHANNELS * MAX_BLENDED;
	SMotionPlaybackState states[MAX_CHANNELS * MAX_BLENDED];
	int state_count = 0;
	if (!pose->get_motion_playback_states(states, state_capacity, state_count))
		return false;

	bool callbacks_changed = false;
	auto consume_completion = [&](MotionID& current, u8 group, bool& ended) {
		if (!current.valid())
			return;
		for (int i = 0; i < state_count; ++i)
		{
			SMotionPlaybackState& state = states[i];
			if (state.controller_group != group || state.id != current || !state.callback_enabled ||
				state.blend_state == eMotionPlaybackBlendFalloff || !state.stop_at_end || state.playing)
				continue;
			state.callback_enabled = false;
			ended = true;
			current.invalidate();
			callbacks_changed = true;
			return;
		}
	};
	consume_completion(m_cpu_global_motion, 3, m_global_animation_end);
	consume_completion(m_cpu_legs_motion, 0, m_legs_animation_end);
	consume_completion(m_cpu_torso_motion, 1, m_torso_animation_end);
	if (callbacks_changed)
	{
		if (!pose->set_motion_playback_states(states, state_count))
			return false;
		check_callbacks();
		if (!m_data.global.get_motion().valid())
			selection.select_animation();
		if (!pose->get_motion_playback_states(states, state_capacity, state_count))
			return false;
	}

	MotionID next_global = m_cpu_global_motion;
	MotionID next_legs = m_cpu_legs_motion;
	MotionID next_torso = m_cpu_torso_motion;
	SAnimationPart* activate_after_commit[3] = {nullptr, nullptr, nullptr};
	int activate_count = 0;
	bool changed = false;
	bool global_started = false;

	auto update_part = [&](SAnimationPart& part, MotionID& current, u8 group) {
		const MotionID selected = part.get_motion();
		bool current_active = false;
		bool current_found = false;
		for (int i = 0; i < state_count; ++i)
		{
			if (states[i].controller_group == group && states[i].id == current)
			{
				current_found = true;
				current_active = current_active || states[i].blend_state != eMotionPlaybackBlendFalloff;
			}
		}

		if (!selected.valid())
		{
			bool needs_falloff = current.valid();
			for (int i = 0; i < state_count; ++i)
			{
				SMotionPlaybackState& state = states[i];
				if (state.controller_group != group)
					continue;
				if (state.blend_state != eMotionPlaybackBlendFalloff || state.callback_enabled)
				{
					state.blend_state = eMotionPlaybackBlendFalloff;
					state.callback_enabled = false;
					needs_falloff = true;
				}
			}
			if (needs_falloff)
			{
				current.invalidate();
				changed = true;
				if (activate_count < 3)
					activate_after_commit[activate_count++] = &part;
			}
			return true;
		}

		if (selected == current && current_found && current_active && part.actual)
		{
			SMotionPlaybackState definition;
			definition.id = selected;
			if (!pose->configure_motion_playback_state(definition))
				return false;
			const float requested_speed = group == 3 && m_data.get_speed() > 0.f ? m_data.get_speed() :
				definition.speed;
			for (int i = 0; i < state_count; ++i)
			{
				SMotionPlaybackState& state = states[i];
				if (state.controller_group == group && state.id == selected && state.speed != requested_speed)
				{
					state.speed = requested_speed;
					changed = true;
				}
			}
			return true;
		}

		SMotionPlaybackState next_state;
		next_state.id = selected;
		next_state.channel = 0;
		next_state.controller_group = group;
		u16 partition = 0;
		if (!pose->motion_partition(selected, partition))
			next_state.partition_mask = (1u << MAX_PARTS) - 1;
		if (!pose->configure_motion_playback_state(next_state))
			return false;
		next_state.weight = EPS_S;
		next_state.blend_state = eMotionPlaybackBlendAccrue;
		if (group == 3 && m_data.get_speed() > 0.f)
			next_state.speed = m_data.get_speed();

		for (int i = 0; i < state_count; ++i)
		{
			SMotionPlaybackState& state = states[i];
			if (state.controller_group != group)
				continue;
			state.blend_state = eMotionPlaybackBlendFalloff;
			state.blend_falloff = next_state.blend_falloff;
			state.callback_enabled = false;
		}

		int channel_state_count = 0;
		for (int i = 0; i < state_count; ++i)
			channel_state_count += states[i].channel == next_state.channel;
		while (channel_state_count >= MAX_BLENDED || state_count >= state_capacity)
		{
			int lightest_fade = -1;
			float smallest_weight = 2.f;
			for (int i = 0; i < state_count; ++i)
			{
				if (states[i].channel == next_state.channel &&
					states[i].blend_state == eMotionPlaybackBlendFalloff && states[i].weight < smallest_weight)
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

		states[state_count++] = next_state;
		current = selected;
		changed = true;
		if (group == 3)
			global_started = true;
		if (activate_count < 3)
			activate_after_commit[activate_count++] = &part;
		return true;
	};

	if (!update_part(m_data.global, next_global, 3) || !update_part(m_data.legs, next_legs, 0) ||
		!update_part(m_data.torso, next_torso, 1))
		return false;
	if (changed && !pose->set_motion_playback_states(states, state_count))
		return false;

	m_cpu_global_motion = next_global;
	m_cpu_legs_motion = next_legs;
	m_cpu_torso_motion = next_torso;
	for (int i = 0; i < activate_count; ++i)
	{
		activate_after_commit[i]->actual = true;
		if (activate_after_commit[i]->get_motion().valid())
		{
			ANIMATION_EVENT_MAP_IT found = m_anim_events.find(activate_after_commit[i]->get_motion());
			if (found != m_anim_events.end())
				for (ANIMATION_EVENT_VEC_IT event = found->second.begin(); event != found->second.end(); ++event)
					event->handled = false;
		}
	}
	if (global_started)
	{
		m_man->notify(ControlCom::eventAnimationStart, 0);
	}

	SMotionPlaybackState active_states[MAX_CHANNELS * MAX_BLENDED];
	int active_count = 0;
	if (!pose->get_motion_playback_states(active_states, state_capacity, active_count))
		return false;
	auto check_cpu_events = [&](const SAnimationPart& part, u8 group) {
		if (!part.get_motion().valid())
			return;
		for (int i = 0; i < active_count; ++i)
		{
			const SMotionPlaybackState& state = active_states[i];
			if (state.controller_group != group || state.id != part.get_motion() || state.time_total <= EPS_S)
				continue;
			ANIMATION_EVENT_MAP_IT found = m_anim_events.find(state.id);
			if (found == m_anim_events.end())
				return;
			const float current_percent = state.time_current / state.time_total;
			for (ANIMATION_EVENT_VEC_IT event = found->second.begin(); event != found->second.end(); ++event)
			{
				if (event->handled || event->time_perc >= current_percent)
					continue;
				event->handled = true;
				SAnimationSignalEventData event_data(state.id, event->time_perc, event->event_id);
				m_man->notify(ControlCom::eventAnimationSignal, &event_data);
			}
			return;
		}
	};
	check_cpu_events(m_data.global, 3);
	check_cpu_events(m_data.legs, 0);
	check_cpu_events(m_data.torso, 1);
	return changed || callbacks_changed;
}

void CControlAnimation::restart(SAnimationPart& part, PlayCallback callback)
{
	VERIFY(part.get_motion().valid());
	VERIFY(part.blend);

	u16 bone_or_part = m_skeleton_animated->LL_GetMotionDef(part.get_motion())->bone_or_part;
	if (bone_or_part == u16(-1)) bone_or_part = m_skeleton_animated->LL_PartID("default");

	//save 
	float time_saved = part.blend->timeCurrent;

	// start
	part.blend = m_skeleton_animated->LL_PlayCycle(bone_or_part, part.get_motion(), TRUE, callback, this);

	// restore
	part.blend->timeCurrent = time_saved;
}

void CControlAnimation::restart()
{
	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());

	if (m_data.global.blend) restart(m_data.global, global_animation_end_callback);
	if (m_data.legs.blend) restart(m_data.legs, legs_animation_end_callback);
	if (m_data.torso.blend) restart(m_data.torso, torso_animation_end_callback);
}

void CControlAnimation::freeze()
{
	if (m_freeze) return;
	m_freeze = true;

	if (m_data.global.blend)
	{
		m_saved_global_speed = m_data.global.blend->speed;
		m_data.global.blend->speed = 0.f;
	}
	if (m_data.legs.blend)
	{
		m_saved_legs_speed = m_data.legs.blend->speed;
		m_data.legs.blend->speed = 0.f;
	}
	if (m_data.torso.blend)
	{
		m_saved_torso_speed = m_data.torso.blend->speed;
		m_data.torso.blend->speed = 0.f;
	}
}

void CControlAnimation::unfreeze()
{
	if (!m_freeze) return;
	m_freeze = false;

	if (m_data.global.blend)
	{
		m_data.global.blend->speed = m_saved_global_speed;
	}
	if (m_data.legs.blend)
	{
		m_data.legs.blend->speed = m_saved_legs_speed;
	}
	if (m_data.torso.blend)
	{
		m_data.torso.blend->speed = m_saved_torso_speed;
	}
}
