//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "SkeletonMotions.h"
//#include "SkeletonAnimated.h"
#include "Fmesh.h"
#include "motion.h"
#include "..\Include\xrRender\Kinematics.h"

motions_container* g_pMotionsContainer = 0;

u16 CPartition::part_id(const shared_str& name) const
{
	for (u16 i = 0; i < MAX_PARTS; ++i)
	{
		const CPartDef* pd = part(i);
		if (pd && pd->Name == name)
			return i;
	}
	Msg("!there is no part named [%s]", name.c_str());
	return u16(-1);
}

void CPartition::load(IKinematics* V, LPCSTR model_name)
{
	string_path fn, fn_full;
	xr_strcpy(fn, sizeof(fn), model_name);
	if (strext(fn))
		*strext(fn) = 0;
	xr_strcat(fn, sizeof(fn), ".ltx");

	FS.update_path(fn_full, "$game_meshes$", fn);

	CInifile ini(fn_full, TRUE, TRUE, FALSE);

	if (ini.sections().size() == 0) return;
	shared_str part_name = "partition_name";
	for (u32 i = 0; i < MAX_PARTS; ++i)
	{
		string64 buff;
		xr_sprintf(buff, sizeof(buff), "part_%d", i);

		CInifile::Sect S = ini.r_section(buff);
		CInifile::SectCIt it = S.Data.begin();
		CInifile::SectCIt it_e = S.Data.end();

		if (!S.Data.size()) continue;

		while (!P[i]) create();
		P[i]->bones.clear_not_free();

		for (; it != it_e; ++it)
		{
			const CInifile::Item& I = *it;
			if (I.first == part_name)
			{
				P[i]->Name = I.second;
			}
			else
			{
				u32 bid = V->LL_BoneID(I.first.c_str());
				P[i]->bones.push_back(bid);
			}
		}
	}
}

u16 find_bone_id(vecBones* bones, shared_str nm)
{
	for (u16 i = 0; i < (u16)bones->size(); i++)
		if (bones->at(i)->name == nm) return i;
	return BI_NONE;
}

//-----------------------------------------------------------------------
BOOL motions_value::load(LPCSTR N, IReader* data, vecBones* bones)
{
	m_id = N;

	bool bRes = true;
	// Load definitions
	U16Vec rm_bones(bones->size(), BI_NONE);
	IReader* MP = data->open_chunk(OGF_S_SMPARAMS);

	if (MP)
	{
		u16 vers = MP->r_u16();
		u16 part_bone_cnt = 0;
		string128 buf;
		R_ASSERT3(vers <= xrOGF_SMParamsVersion, "Invalid OGF/OMF version:", N);

		// partitions
		u16 part_count = MP->r_u16();

		for (u16 part_i = 0; part_i < part_count; part_i++)
		{
			CPartDef* PART = m_partition[part_i];
			while (!PART) PART = m_partition.create();
			MP->r_stringZ(buf, sizeof(buf));
			PART->Name = _strlwr(buf);
			PART->bones.resize(MP->r_u16());

			for (xr_vector<u32>::iterator b_it = PART->bones.begin(); b_it < PART->bones.end(); b_it++)
			{
				MP->r_stringZ(buf, sizeof(buf));
				u16 m_idx = u16(MP->r_u32());
				*b_it = find_bone_id(bones, buf);
#ifdef _EDITOR
                if (*b_it==BI_NONE )
                {
                    bRes = false;
                    Msg ("!Can't find bone: '%s'", buf);
                }

                if (rm_bones.size() <= m_idx)
                {
                    bRes = false;
                    Msg ("!Can't load: '%s' invalid bones count", N);
                }
#else
				VERIFY3(*b_it != BI_NONE, "Can't find bone:", buf);
#endif
				if (bRes) rm_bones[m_idx] = u16(*b_it);
			}
			part_bone_cnt = u16(part_bone_cnt + (u16)PART->bones.size());
		}

#ifdef _EDITOR
        if (part_bone_cnt!=(u16)bones->size())
        {
            bRes = false;
            Msg("!Different bone count[%s] [Object: '%d' <-> Motions: '%d']", N, bones->size(),part_bone_cnt);
        }
#else
		VERIFY3(part_bone_cnt == (u16)bones->size(), "Different bone count '%s'", N);
#endif
		if (bRes)
		{
			// motion defs (cycle&fx)
			u16 mot_count = MP->r_u16();
			m_mdefs.resize(mot_count);

			for (u16 mot_i = 0; mot_i < mot_count; mot_i++)
			{
				MP->r_stringZ(buf, sizeof(buf));
				shared_str nm = _strlwr(buf);
				u32 dwFlags = MP->r_u32();
				CMotionDef& D = m_mdefs[mot_i];
				D.Load(MP, dwFlags, vers);
				//. m_mdefs.push_back (D);

				if (dwFlags & esmFX)
					m_fx.insert(mk_pair(nm, mot_i));
				else
					m_cycle.insert(mk_pair(nm, mot_i));

				m_motion_map.insert(mk_pair(nm, mot_i));
			}
		}
		MP->close();
	}
	else
	{
		Debug.fatal(DEBUG_INFO, "Old skinned model version unsupported! (%s)", N);
	}
	if (!bRes) return false;

	// Load animation
	IReader* MS = data->open_chunk(OGF_S_MOTIONS);
	if (!MS) return false;

	u32 dwCNT = 0;
	MS->r_chunk_safe(0, &dwCNT, sizeof(dwCNT));
	VERIFY(dwCNT < 0x3FFF); // MotionID 2 bit - slot, 14 bit - motion index

	// set per bone motion size
	for (u32 i = 0; i < bones->size(); i++)
		m_motions[bones->at(i)->name].resize(dwCNT);

	// load motions
	for (u16 m_idx = 0; m_idx < (u16)dwCNT; m_idx++)
	{
		string128 mname;
		R_ASSERT(MS->find_chunk(m_idx + 1));
		MS->r_stringZ(mname, sizeof(mname));
#ifdef _DEBUG
        // sanity check
        xr_strlwr (mname);
        accel_map::iterator I= m_motion_map.find(mname);
        VERIFY3 (I!=m_motion_map.end(),"Can't find motion:",mname);
        VERIFY3 (I->second==m_idx,"Invalid motion index:",mname);
#endif
		u32 dwLen = MS->r_u32();
		for (u32 i = 0; i < bones->size(); i++)
		{
			u16 bone_id = rm_bones[i];
			VERIFY2(bone_id != BI_NONE, "Invalid remap index.");

			if ((bones->size() - 1) < bone_id)
			{
				MS->close();
				return false;
			}

			CMotion& M = m_motions[bones->at(bone_id)->name][m_idx];
			M.set_count(dwLen);
			M.set_flags(MS->r_u8());

			if (M.test_flag(flRKeyAbsent))
			{
				CKeyQR* r = (CKeyQR*)MS->pointer();
				u32 crc_q = crc32(r, sizeof(CKeyQR));
				M._keysR.create(crc_q, 1, r);
				MS->advance(1 * sizeof(CKeyQR));
			}
			else
			{
				u32 crc_q = MS->r_u32();
				M._keysR.create(crc_q, dwLen, (CKeyQR*)MS->pointer());
				MS->advance(dwLen * sizeof(CKeyQR));
			}
			if (M.test_flag(flTKeyPresent))
			{
				u32 crc_t = MS->r_u32();
				if (M.test_flag(flTKey16IsBit))
				{
					M._keysT16.create(crc_t, dwLen, (CKeyQT16*)MS->pointer());
					MS->advance(dwLen * sizeof(CKeyQT16));
				}
				else
				{
					M._keysT8.create(crc_t, dwLen, (CKeyQT8*)MS->pointer());
					MS->advance(dwLen * sizeof(CKeyQT8));
				};

				MS->r_fvector3(M._sizeT);
				MS->r_fvector3(M._initT);
			}
			else
			{
				MS->r_fvector3(M._initT);
			}
		}
	}
	// Msg("Motions %d/%d %4d/%4d/%d, %s",p_cnt,m_cnt, m_load,m_total,m_r,N);
	MS->close();

	return bRes;
}

MotionVec* motions_value::bone_motions(shared_str bone_name)
{
	BoneMotionMapIt I = m_motions.find(bone_name);
	// VERIFY (I != m_motions.end());
	if (I == m_motions.end())
		return (0);

	return (&(*I).second);
}

//-----------------------------------
motions_container::motions_container()
{
}

//extern shared_str s_bones_array_const;
motions_container::~motions_container()
{
	// clean (false);
	// clean (true);
	// dump ();
	VERIFY(container.empty());
	// Igor:
	//s_bones_array_const = 0;
}

bool motions_container::has(shared_str key)
{
	return (container.find(key) != container.end());
}

motions_value* motions_container::dock(shared_str key, IReader* data, vecBones* bones)
{
	return dock(key, key, data, bones);
}

motions_value* motions_container::dock(shared_str cache_key, shared_str motion_name, IReader* data, vecBones* bones)
{
	motions_value* result = 0;
	SharedMotionsMapIt I = container.find(cache_key);
	if (I != container.end()) result = I->second;
	if (0 == result)
	{
		// loading motions
		VERIFY(data);
		result = xr_new<motions_value>();
		result->m_dwReference = 0;
		BOOL bres = result->load(motion_name.c_str(), data, bones);
		if (bres)
			container.insert(mk_pair(cache_key, result));
		else
			xr_delete(result);
	}
	return result;
}

void motions_container::clean(bool force_destroy)
{
	SharedMotionsMapIt it = container.begin();
	SharedMotionsMapIt _E = container.end();
	if (force_destroy)
	{
		for (; it != _E; it++)
		{
			motions_value* sv = it->second;
			xr_delete(sv);
		}
		container.clear();
	}
	else
	{
		for (; it != _E;)
		{
			motions_value* sv = it->second;
			if (0 == sv->m_dwReference)
			{
				SharedMotionsMapIt i_current = it;
				SharedMotionsMapIt i_next = ++it;
				xr_delete(sv);
				container.erase(i_current);
				it = i_next;
			}
			else
			{
				it++;
			}
		}
	}
}

void motions_container::dump()
{
	SharedMotionsMapIt it = container.begin();
	SharedMotionsMapIt _E = container.end();
	Log("--- motion container --- begin:");
	u32 sz = sizeof(*this);
	for (u32 k = 0; it != _E; k++, it++)
	{
		sz += it->second->mem_usage();
		Msg("#%3d: [%3d/%5d Kb] - %s", k, it->second->m_dwReference, it->second->mem_usage() / 1024, it->first.c_str());
	}
	Msg("--- items: %d, mem usage: %d Kb ", container.size(), sz / 1024);
	Log("--- motion container --- end.");
}

shared_str make_motion_cache_key(shared_str motion_name, const vecBones& bones)
{
	VERIFY(motion_name);
	xr_string normalized_name = motion_name.c_str();
	xr_strlwr(normalized_name);
	for (xr_string::iterator it = normalized_name.begin(); it != normalized_name.end(); ++it)
		if (*it == '/')
			*it = '\\';

	xr_string key;
	char length[32];
	xr_sprintf(length, sizeof(length), "%u:", static_cast<u32>(normalized_name.size()));
	key.append(length);
	key.append(normalized_name);
	key += '|';
	xr_sprintf(length, sizeof(length), "%u|", static_cast<u32>(bones.size()));
	key.append(length);

	for (const CBoneData* bone : bones)
	{
		VERIFY(bone);
		xr_string normalized_bone_name = bone->name.c_str();
		xr_strlwr(normalized_bone_name);
		xr_sprintf(length, sizeof(length), "%u:", static_cast<u32>(normalized_bone_name.size()));
		key.append(length);
		key.append(normalized_bone_name);
	}

	return shared_str(key.c_str());
}

//////////////////////////////////////////////////////////////////////////
// High level control
void CMotionDef::Load(IReader* MP, u32 fl, u16 version)
{
	// params
	bone_or_part = MP->r_u16(); // bCycle?part_id:bone_id;
	motion = MP->r_u16(); // motion_id
	speed = Quantize(MP->r_float());
	power = Quantize(MP->r_float());
	accrue = Quantize(MP->r_float());
	falloff = Quantize(MP->r_float());
	flags = (u16)fl;
	if (!(flags & esmFX) && (falloff >= accrue)) falloff = u16(accrue - 1);

	if (version >= 4)
	{
		u32 cnt = MP->r_u32();
		if (cnt > 0)
		{
			marks.resize(cnt);

			for (u32 i = 0; i < cnt; ++i)
				marks[i].Load(MP);
		}
	}
}

bool CMotionDef::StopAtEnd()
{
	return !!(flags & esmStopAtEnd);
}

bool shared_motions::create(shared_str key, IReader* data, vecBones* bones)
{
	if (!g_pMotionsContainer)
	{
		destroy();
		p_ = nullptr;
		return false;
	}
	return create(key, data, bones, *g_pMotionsContainer);
}

bool shared_motions::create(shared_str key, IReader* data, vecBones* bones, motions_container& cache)
{
	return create(key, key, data, bones, cache);
}

bool shared_motions::create(shared_str cache_key, shared_str motion_name, IReader* data, vecBones* bones, motions_container& cache)
{
	motions_value* v = cache.dock(cache_key, motion_name, data, bones);
	if (0 != v)
		v->m_dwReference++;
	destroy();
	p_ = v;
	return (0 != v);
}

bool shared_motions::create_from_vfs(shared_str motion_name, vecBones* bones, motions_container& cache)
{
	if (!motion_name || !bones || bones->empty())
	{
		destroy();
		p_ = nullptr;
		return false;
	}

	string_path normalized_name;
	xr_strcpy(normalized_name, sizeof(normalized_name), motion_name.c_str());
	if (!strext(normalized_name))
		xr_strcat(normalized_name, sizeof(normalized_name), ".omf");
	xr_strlwr(normalized_name);
	for (char* it = normalized_name; *it; ++it)
		if (*it == '/')
			*it = '\\';

	const shared_str canonical_name(normalized_name);
	const shared_str cache_key = make_motion_cache_key(canonical_name, *bones);
	if (cache.has(cache_key))
		return create(cache_key, canonical_name, nullptr, bones, cache);

	string_path resolved_path;
	if (!FS.exist(resolved_path, "$level$", canonical_name.c_str()) &&
		!FS.exist(resolved_path, "$game_meshes$", canonical_name.c_str()))
	{
		destroy();
		p_ = nullptr;
		return false;
	}

	IReader* data = FS.r_open(resolved_path);
	if (!data)
	{
		destroy();
		p_ = nullptr;
		return false;
	}

	const bool result = create(cache_key, canonical_name, data, bones, cache);
	FS.r_close(data);
	return result;
}

const CMotion* shared_motions::bone_motion(const shared_str& bone_name, u16 motion_index) const
{
	if (!p_)
		return nullptr;
	const auto it = p_->m_motions.find(bone_name);
	if (it == p_->m_motions.end() || motion_index >= it->second.size())
		return nullptr;
	return &it->second[motion_index];
}

bool ResolveMotionPlaybackTrack(const shared_motions* slots, u16 slot_count, const shared_str& bone_name,
	const SMotionPlaybackState& state, SMotionBlendTrack& track)
{
	if (!slots || !state.id.valid() || state.weight <= EPS_S || state.id.slot >= slot_count ||
		state.channel >= MAX_CHANNELS)
		return false;
	const CMotion* motion = slots[state.id.slot].bone_motion(bone_name, state.id.idx);
	if (!motion)
		return false;
	track.motion = motion;
	track.time_seconds = state.time_current;
	track.weight = state.weight;
	return true;
}

bool EvaluateMotionBoneFromStates(CKey& result, const shared_motions* slots, u16 slot_count,
	const shared_str& bone_name, const SMotionPlaybackState* states, int state_count,
	const SMotionChannelDef* channel_definitions, int channel_count)
{
	if (channel_count <= 0 || channel_count > static_cast<int>(MAX_CHANNELS) || state_count < 0 ||
		(state_count > 0 && !states) || !channel_definitions)
		return false;

	SMotionBlendTrack tracks[MAX_CHANNELS][MAX_BLENDED];
	SMotionChannelTrackSet channels[MAX_CHANNELS];
	for (int channel = 0; channel < channel_count; ++channel)
	{
		channels[channel].tracks = tracks[channel];
		channels[channel].count = 0;
		channels[channel].definition = channel_definitions[channel];
	}

	for (int state_index = 0; state_index < state_count; ++state_index)
	{
		const SMotionPlaybackState& state = states[state_index];
		if (state.weight <= EPS_S)
			continue;
		if (state.channel >= channel_count)
			return false;

		SMotionBlendTrack track;
		if (!ResolveMotionPlaybackTrack(slots, slot_count, bone_name, state, track))
			return false;

		SMotionChannelTrackSet& channel = channels[state.channel];
		if (channel.count >= static_cast<int>(MAX_BLENDED))
			return false;
		tracks[state.channel][channel.count++] = track;
	}

	CKey evaluated;
	EvaluateMotionBone(evaluated, channels, channel_count);
	result = evaluated;
	return true;
}

bool shared_motions::create(shared_motions const& rhs)
{
	motions_value* v = rhs.p_;
	if (0 != v)
		v->m_dwReference++;
	destroy();
	p_ = v;
	return (0 != v);
}

const motion_marks::interval* motion_marks::pick_mark(const float& t) const
{
	C_ITERATOR it = intervals.begin();
	C_ITERATOR it_e = intervals.end();

	for (; it != it_e; ++it)
	{
		const interval& I = (*it);
		if (I.first <= t && I.second >= t)
			return &I;

		if (I.first > t)
			break;
	}
	return NULL;
}

bool motion_marks::is_mark_between(float const& t0, float const& t1) const
{
	VERIFY(t0 <= t1);

	C_ITERATOR i = intervals.begin();
	C_ITERATOR e = intervals.end();
	for (; i != e; ++i)
	{
		VERIFY((*i).first <= (*i).second);

		if ((*i).first == t0)
			return (true);

		if ((*i).first > t0)
		{
			if ((*i).second <= t1)
				return (true);

			if ((*i).first <= t1)
				return (true);

			return (false);
		}

		if ((*i).second < t0)
			continue;

		if ((*i).second == t0)
			return (true);

		return (true);
	}

	return (false);
}

float motion_marks::time_to_next_mark(float time) const
{
	C_ITERATOR i = intervals.begin();
	C_ITERATOR e = intervals.end();
	float result_dist = FLT_MAX;
	for (; i != e; ++i)
	{
		float dist = (*i).first - time;
		if (dist > 0.f && dist < result_dist)
			result_dist = dist;
	}
	return result_dist;
}

void ENGINE_API motion_marks::Load(IReader* R)
{
	xr_string tmp;
	R->r_string(tmp);
	name = tmp.c_str();
	u32 cnt = R->r_u32();
	intervals.resize(cnt);
	for (u32 i = 0; i < cnt; ++i)
	{
		interval& item = intervals[i];
		item.first = R->r_float();
		item.second = R->r_float();
	}
}
#ifdef _EDITOR
void motion_marks::Save(IWriter* W)
{
    W->w_string (name.c_str());
    u32 cnt = intervals.size();
    W->w_u32 (cnt);
    for(u32 i=0; i<cnt; ++i)
    {
        interval& item = intervals[i];
        W->w_float (item.first);
        W->w_float (item.second);
    }
}
#endif
