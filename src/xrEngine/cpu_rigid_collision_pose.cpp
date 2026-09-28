#include "stdafx.h"
#pragma hdrstop

#include "cpu_rigid_collision_pose.h"
#include "Fmesh.h"
#include "bone.h"
#include "motion.h"
#include "SkeletonMotions.h"
#include "xr_object_list.h"

namespace
{
const u32 CpuMaxBoneCount = 64;
const u32 CpuMaxMotionSlots = 48;
const u32 CpuMaxMotionReferences = 4096;

struct SCpuBonePose
{
	Fmatrix transform;
};

class CCpuSkeletonCollisionPose : public IObjectCollisionPose
{
protected:
	vecBones m_bones;
	xr_vector<SCpuBonePose> m_bone_pose;
	xr_vector<SCpuBonePose> m_next_bone_pose;
	xr_vector<SCpuBonePose> m_bind_pose;
	xr_vector<u8> m_bone_calculation_state;
	u64 m_visible_bones = 0;
	u16 m_root_bone_id = BI_NONE;
	Fbox m_bounds_box;
	Fsphere m_bounds_sphere;

	bool LoadSkeleton(IReader& data)
	{
		IReader* names = data.open_chunk(OGF_S_BONE_NAMES);
		if (!names)
			return false;

		const u32 count = names->r_u32();
		if (count == 0 || count > CpuMaxBoneCount)
		{
			names->close();
			return false;
		}

		m_bones.reserve(count);
		xr_vector<shared_str> parent_names;
		parent_names.reserve(count);
		for (u32 i = 0; i < count; ++i)
		{
			CBoneData* bone = xr_new<CBoneData>(static_cast<u16>(i));
			bone->SetParentID(BI_NONE);
			string256 name;
			string256 parent;
			names->r_stringZ(name, sizeof(name));
			names->r_stringZ(parent, sizeof(parent));
			xr_strlwr(name);
			xr_strlwr(parent);
			bone->name = name;
			names->r(&bone->obb, sizeof(bone->obb));
			m_bones.push_back(bone);
			parent_names.push_back(parent);
		}
		names->close();

		IReader* ik_data = data.open_chunk(OGF_S_IKDATA);
		if (!ik_data)
			return false;

		for (CBoneData* bone : m_bones)
		{
			const u16 version = static_cast<u16>(ik_data->r_u32());
			ik_data->r_stringZ(bone->game_mtl_name);
			ik_data->r(&bone->shape, sizeof(bone->shape));
			if (!bone->IK_data.Import(*ik_data, version))
			{
				ik_data->close();
				return false;
			}

			Fvector rotation;
			Fvector translation;
			ik_data->r_fvector3(rotation);
			ik_data->r_fvector3(translation);
			bone->bind_transform.setXYZi(rotation);
			bone->bind_transform.translate_over(translation);
			bone->mass = ik_data->r_float();
			ik_data->r_fvector3(bone->center_of_mass);
		}
		ik_data->close();

		u16 root_id = BI_NONE;
		for (u16 id = 0; id < m_bones.size(); ++id)
		{
			CBoneData& bone = *m_bones[id];
			const shared_str& parent_name = parent_names[id];
			if (!parent_name || !parent_name.c_str()[0])
			{
				if (root_id != BI_NONE)
					return false;
				root_id = id;
				continue;
			}

			for (u16 parent_id = 0; parent_id < m_bones.size(); ++parent_id)
			{
				if (m_bones[parent_id]->name == parent_name)
				{
					bone.SetParentID(parent_id);
					break;
				}
			}
			if (bone.GetParentID() == BI_NONE)
				return false;
		}
		if (root_id == BI_NONE)
			return false;
		m_root_bone_id = root_id;

		m_bone_pose.resize(m_bones.size());
		m_next_bone_pose.resize(m_bones.size());
		m_bind_pose.resize(m_bones.size());
		m_bone_calculation_state.resize(m_bones.size());
		if (!CalculateBoneTransforms(nullptr, m_bind_pose))
			return false;
		m_bone_pose = m_bind_pose;
		m_visible_bones = m_bones.size() == CpuMaxBoneCount ? u64(-1) : ((u64(1) << m_bones.size()) - 1);
		CalculateBounds();
		return true;
	}

	bool CalculateBoneTransforms(const Fmatrix* local_transforms, xr_vector<SCpuBonePose>& output)
	{
		std::fill(m_bone_calculation_state.begin(), m_bone_calculation_state.end(), 0);
		for (u16 id = 0; id < m_bones.size(); ++id)
			if (!CalculateBoneTransform(id, local_transforms, output, m_bone_calculation_state))
				return false;
		return true;
	}

	bool CalculateBoneTransform(u16 id, const Fmatrix* local_transforms, xr_vector<SCpuBonePose>& output,
		xr_vector<u8>& calculated)
	{
		if (calculated[id] == 2)
			return true;
		if (calculated[id] == 1)
			return false;

		calculated[id] = 1;
		const CBoneData& bone = *m_bones[id];
		const Fmatrix& local = local_transforms ? local_transforms[id] : bone.bind_transform;
		const u16 parent_id = bone.GetParentID();
		if (parent_id == BI_NONE)
			output[id].transform.mul_43(Fidentity, local);
		else
		{
			if (!CalculateBoneTransform(parent_id, local_transforms, output, calculated))
				return false;
			output[id].transform.mul_43(output[parent_id].transform, local);
		}
		calculated[id] = 2;
		return true;
	}

	void CalculateBounds()
	{
		m_bounds_box.invalidate();
		const auto include_obb = [this](const Fobb& obb, const Fmatrix& bone_transform)
		{
			Fmatrix box_transform;
			Fmatrix bone_to_model;
			obb.xform_get(box_transform);
			bone_to_model.mul_43(bone_transform, box_transform);

			const Fvector& half_size = obb.m_halfsize;
			for (u32 corner = 0; corner < 8; ++corner)
			{
				Fvector point;
				point.set(
					(corner & 1) ? half_size.x : -half_size.x,
					(corner & 2) ? half_size.y : -half_size.y,
					(corner & 4) ? half_size.z : -half_size.z);
				bone_to_model.transform_tiny(point);
				m_bounds_box.modify(point);
			}
		};

		for (u16 id = 0; id < m_bones.size(); ++id)
		{
			const CBoneData& bone = *m_bones[id];
			const Fmatrix& transform = m_bone_pose[id].transform;
			include_obb(bone.obb, transform);
			switch (bone.shape.type)
			{
			case SBoneShape::stBox:
				include_obb(bone.shape.box, transform);
				break;
			case SBoneShape::stSphere:
				{
					Fvector center;
					transform.transform_tiny(center, bone.shape.sphere.P);
					const float radius = bone.shape.sphere.R;
					m_bounds_box.modify(center.x - radius, center.y - radius, center.z - radius);
					m_bounds_box.modify(center.x + radius, center.y + radius, center.z + radius);
				}
				break;
			case SBoneShape::stCylinder:
				{
					Fvector center;
					Fvector direction;
					transform.transform_tiny(center, bone.shape.cylinder.m_center);
					transform.transform_dir(direction, bone.shape.cylinder.m_direction);
					direction.normalize_safe();
					const float half_height = bone.shape.cylinder.m_height * 0.5f;
					const float radius = bone.shape.cylinder.m_radius;
					Fvector extent;
					extent.set(
						_abs(direction.x) * half_height + _sqrt(_max(0.f, 1.f - direction.x * direction.x)) * radius,
						_abs(direction.y) * half_height + _sqrt(_max(0.f, 1.f - direction.y * direction.y)) * radius,
						_abs(direction.z) * half_height + _sqrt(_max(0.f, 1.f - direction.z * direction.z)) * radius);
					Fvector min;
					Fvector max;
					min.sub(center, extent);
					max.add(center, extent);
					m_bounds_box.modify(min);
					m_bounds_box.modify(max);
				}
				break;
			}
		}
		m_bounds_box.getsphere(m_bounds_sphere.P, m_bounds_sphere.R);
	}

public:
	~CCpuSkeletonCollisionPose()
	{
		for (CBoneData*& bone : m_bones)
			xr_delete(bone);
	}

	const Fbox& bounds_box() const override { return m_bounds_box; }
	const Fsphere& bounds_sphere() const override { return m_bounds_sphere; }
	void calculate_pose() override {}
	u64 visible_bones() override { return m_visible_bones; }
	u16 bone_count() const override { return static_cast<u16>(m_bones.size()); }
	BOOL bone_visible(u16 bone_id) override { return bone_id < m_bones.size(); }
	const SBoneShape& bone_shape(u16 bone_id) override { return m_bones[bone_id]->shape; }
	const Fmatrix& bone_transform(u16 bone_id) override { return m_bone_pose[bone_id].transform; }
	u16 bone_id(LPCSTR name) const override
	{
		if (!name || !name[0])
			return u16(-1);

		string256 normalized_name;
		xr_strcpy(normalized_name, name);
		xr_strlwr(normalized_name);
		for (u16 id = 0; id < m_bones.size(); ++id)
			if (xr_strcmp(normalized_name, m_bones[id]->name.c_str()) == 0)
				return id;
		return u16(-1);
	}
};

class CCpuRigidCollisionPose : public CCpuSkeletonCollisionPose
{
public:
	static CCpuRigidCollisionPose* Create(IReader& data)
	{
		CCpuRigidCollisionPose* pose = xr_new<CCpuRigidCollisionPose>();
		if (!pose->LoadSkeleton(data))
		{
			xr_delete(pose);
			return nullptr;
		}
		return pose;
	}
};

class CCpuAnimatedCollisionPose : public CCpuSkeletonCollisionPose
{
	motions_container* m_motion_cache;
	xr_vector<shared_motions> m_motion_slots;
	xr_vector<SMotionPlaybackState> m_playback_states;
	xr_vector<SMotionPlaybackState> m_filtered_states;
	xr_vector<CKey> m_local_keys;
	xr_vector<Fmatrix> m_local_transforms;
	SMotionChannelDef m_channel_definitions[MAX_CHANNELS];
	bool m_root_motion_extraction_enabled = false;

	bool AppendMotionSlot(LPCSTR name)
	{
		if (m_motion_slots.size() >= CpuMaxMotionSlots || !m_motion_cache || !name || !name[0])
			return false;

		m_motion_slots.emplace_back();
		if (!m_motion_slots.back().create_from_vfs(shared_str(name), &m_bones, *m_motion_cache))
		{
			m_motion_slots.pop_back();
			return false;
		}
		return true;
	}

	bool AppendMotionWildcard(LPCSTR pattern)
	{
		FS_FileSet files;
		FS.file_list(files, "$game_meshes$", FS_ListFiles, pattern);
		FS.file_list(files, "$level$", FS_ListFiles, pattern);
		for (FS_FileSet::iterator it = files.begin(); it != files.end(); ++it)
			if (!AppendMotionSlot(it->name.c_str()))
				return false;
		return true;
	}

	bool AppendMotionReference(LPCSTR reference)
	{
		if (!reference || !reference[0])
			return false;

		string_path name;
		xr_strcpy(name, sizeof(name), reference);
		if (strstr(name, "\\*.omf") || strstr(name, "/*.omf"))
			return AppendMotionWildcard(name);

		if (!strext(name))
			xr_strcat(name, sizeof(name), ".omf");
		if (!AppendMotionSlot(name))
			return false;

		string_path lower_name;
		xr_strcpy(lower_name, sizeof(lower_name), name);
		xr_strlwr(lower_name);
		if (strstr(lower_name, "stalker_animation.omf"))
		{
			FS_FileSet extra_animations;
			FS.file_list(extra_animations, "$game_meshes$", FS_ListFiles,
				"actors\\modded_stalker_animations\\*.omf");
			for (FS_FileSet::iterator it = extra_animations.begin(); it != extra_animations.end(); ++it)
				if (!AppendMotionSlot(it->name.c_str()))
					return false;
		}
		return true;
	}

	bool LoadMotionReferences(IReader& data, LPCSTR model_name)
	{
		IReader* refs = data.open_chunk(OGF_S_MOTION_REFS);
		if (refs)
		{
			string_path items;
			refs->r_stringZ(items, sizeof(items));
			const u32 count = _GetItemCount(items);
			if (count > CpuMaxMotionReferences)
			{
				refs->close();
				return false;
			}
			string_path item;
			for (u32 i = 0; i < count; ++i)
			{
				_GetItem(items, i, item);
				if (!AppendMotionReference(item))
				{
					refs->close();
					return false;
				}
			}
			refs->close();
		}
		else if ((refs = data.open_chunk(OGF_S_MOTION_REFS2)) != nullptr)
		{
			const u32 count = refs->r_u32();
			if (count > CpuMaxMotionReferences)
			{
				refs->close();
				return false;
			}
			string_path item;
			for (u32 i = 0; i < count; ++i)
			{
				refs->r_stringZ(item, sizeof(item));
				if (!AppendMotionReference(item))
				{
					refs->close();
					return false;
				}
			}
			refs->close();
		}
		else
		{
			string_path fallback;
			if (strext(model_name))
				xr_strcpy(fallback, sizeof(fallback), model_name);
			else
				strconcat(sizeof(fallback), fallback, model_name, ".ogf");
			if (!AppendMotionSlot(fallback))
				return false;
		}

		return !m_motion_slots.empty();
	}

	bool Load(IReader& data, LPCSTR model_name, motions_container* motion_cache)
	{
		m_motion_cache = motion_cache;
		if (!m_motion_cache || !LoadSkeleton(data) || !LoadMotionReferences(data, model_name))
			return false;
		m_motion_cache = nullptr;

		m_local_keys.resize(m_bones.size());
		m_local_transforms.resize(m_bones.size());
		m_filtered_states.reserve(MAX_CHANNELS * MAX_BLENDED);
		m_channel_definitions[0] = {1.f, eMotionChannelLerp};
		m_channel_definitions[1] = {1.f, eMotionChannelLerp};
		m_channel_definitions[2] = {1.f, eMotionChannelAdd};
		m_channel_definitions[3] = {1.f, eMotionChannelAdd};
		return true;
	}

	u32 MotionPartitionMask(const SMotionPlaybackState& state)
	{
		if (state.partition_mask)
			return state.partition_mask;
		const CMotionDef* definition = m_motion_slots[state.id.slot].motion_def(state.id.idx);
		return definition->bone_or_part < MAX_PARTS ? (1u << definition->bone_or_part) : 0;
	}

	bool MotionAffectsBone(const SMotionPlaybackState& state, u16 bone_id)
	{
		const u32 partition_mask = MotionPartitionMask(state);
		for (u16 partition_id = 0; partition_id < MAX_PARTS; ++partition_id)
		{
			if (!(partition_mask & (1u << partition_id)))
				continue;
			const CPartDef* partition = m_motion_slots[state.id.slot].partition()->part(partition_id);
			if (partition && std::find(partition->bones.begin(), partition->bones.end(), u32(bone_id)) !=
				partition->bones.end())
				return true;
		}
		return false;
	}

	bool CalculateAnimatedPose()
	{
		bool has_active_state = false;
		for (const SMotionPlaybackState& state : m_playback_states)
			has_active_state = has_active_state || state.weight > EPS_S;
		if (!has_active_state && !m_root_motion_extraction_enabled)
		{
			m_bone_pose = m_bind_pose;
			CalculateBounds();
			return true;
		}
		for (u16 id = 0; id < m_bones.size(); ++id)
		{
			if (!has_active_state)
			{
				m_local_transforms[id] = m_bones[id]->bind_transform;
				if (m_root_motion_extraction_enabled && id == m_root_bone_id)
					m_local_transforms[id] = Fidentity;
				continue;
			}

			m_filtered_states.clear();
			for (const SMotionPlaybackState& state : m_playback_states)
				if (state.weight > EPS_S && MotionAffectsBone(state, id))
					m_filtered_states.push_back(state);

			if (m_filtered_states.empty())
			{
				m_local_transforms[id] = m_bones[id]->bind_transform;
				continue;
			}

			bool has_base_channel = false;
			for (const SMotionPlaybackState& state : m_filtered_states)
				has_base_channel = has_base_channel || state.channel == 0;
			if (!has_base_channel)
				return false;

			if (!EvaluateMotionBoneFromStates(m_local_keys[id], m_motion_slots.data(),
				static_cast<u16>(m_motion_slots.size()), m_bones[id]->name, m_filtered_states.data(),
				static_cast<int>(m_filtered_states.size()), m_channel_definitions, MAX_CHANNELS))
				return false;
			if (!_valid(m_local_keys[id].Q) || !_valid(m_local_keys[id].T))
				return false;
			m_local_transforms[id].mk_xform(m_local_keys[id].Q, m_local_keys[id].T);
			if (m_root_motion_extraction_enabled && id == m_root_bone_id)
				m_local_transforms[id] = Fidentity;
		}

		if (!CalculateBoneTransforms(m_local_transforms.data(), m_next_bone_pose))
			return false;
		m_bone_pose.swap(m_next_bone_pose);
		CalculateBounds();
		return true;
	}

public:
	explicit CCpuAnimatedCollisionPose() : m_motion_cache(nullptr)
	{}

	static CCpuAnimatedCollisionPose* Create(IReader& data, LPCSTR model_name, motions_container* motion_cache)
	{
		CCpuAnimatedCollisionPose* pose = xr_new<CCpuAnimatedCollisionPose>();
		if (!pose->Load(data, model_name, motion_cache))
		{
			xr_delete(pose);
			return nullptr;
		}
		return pose;
	}

	void calculate_pose() override
	{
		// State changes and scheduler ticks evaluate eagerly, so collision reads
		// only the last complete pose and never repeat the blend per query.
	}

	bool find_cycle(LPCSTR name, MotionID& result) override
	{
		result.invalidate();
		if (!name || !name[0])
			return false;
		string_path normalized_name;
		xr_strcpy(normalized_name, sizeof(normalized_name), name);
		xr_strlwr(normalized_name);
		for (int slot = static_cast<int>(m_motion_slots.size()) - 1; slot >= 0; --slot)
		{
			accel_map* cycles = m_motion_slots[slot].cycle();
			const accel_map::iterator found = cycles->find(shared_str(normalized_name));
			if (found == cycles->end())
				continue;
			result.set(static_cast<u16>(slot), found->second);
			return true;
		}
		return false;
	}

	bool find_fx(LPCSTR name, MotionID& result) override
	{
		result.invalidate();
		if (!name || !name[0])
			return false;
		string_path normalized_name;
		xr_strcpy(normalized_name, sizeof(normalized_name), name);
		xr_strlwr(normalized_name);
		for (int slot = static_cast<int>(m_motion_slots.size()) - 1; slot >= 0; --slot)
		{
			accel_map* effects = m_motion_slots[slot].fx();
			const accel_map::iterator found = effects->find(shared_str(normalized_name));
			if (found == effects->end())
				continue;
			result.set(static_cast<u16>(slot), found->second);
			return true;
		}
		return false;
	}

	bool motion_partition(const MotionID& id, u16& partition) const override
	{
		if (!id.valid() || id.slot >= m_motion_slots.size() ||
			id.idx >= m_motion_slots[id.slot].motion_defs()->size())
			return false;

		const CMotionDef* definition = m_motion_slots[id.slot].motion_def(id.idx);
		if (!definition || definition->bone_or_part >= MAX_PARTS)
			return false;
		partition = definition->bone_or_part;
		return true;
	}

	bool motion_uses_root_mover(const MotionID& id) const override
	{
		if (!id.valid() || id.slot >= m_motion_slots.size() ||
			id.idx >= m_motion_slots[id.slot].motion_defs()->size())
			return false;

		const CMotionDef* definition = m_motion_slots[id.slot].motion_def(id.idx);
		return definition && (definition->flags & esmRootMover) != 0;
	}

	bool sample_root_motion_transform(const SMotionPlaybackState& state, Fmatrix& transform) const override
	{
		if (m_root_bone_id >= m_bones.size() || !std::isfinite(state.time_current) || state.time_current < 0.f)
			return false;

		// animation_movement_controller evaluates exactly one blend at full weight
		// on channel 0 to obtain its root curve, independently of body blends.
		SMotionPlaybackState root_state = state;
		root_state.channel = 0;
		root_state.weight = 1.f;
		root_state.blend_state = eMotionPlaybackBlendFixed;
		const shared_str& root_bone_name = m_bones[m_root_bone_id]->name;
		CKey root_key;
		if (!EvaluateMotionBoneFromStates(root_key, m_motion_slots.data(),
			static_cast<u16>(m_motion_slots.size()), root_bone_name, &root_state, 1,
			m_channel_definitions, MAX_CHANNELS))
			return false;

		Fmatrix sampled_transform;
		sampled_transform.mk_xform(root_key.Q, root_key.T);
		if (!_valid(sampled_transform))
			return false;

		transform.set(sampled_transform);
		return true;
	}

	bool set_root_motion_extraction(bool enabled) override
	{
		if (m_root_motion_extraction_enabled == enabled)
			return true;

		const bool previous = m_root_motion_extraction_enabled;
		m_root_motion_extraction_enabled = enabled;
		if (CalculateAnimatedPose())
			return true;

		m_root_motion_extraction_enabled = previous;
		return false;
	}

	bool motion_mark_between(const MotionID& id, u16 mark_index, float time_previous, float time_current) const override
	{
		if (!id.valid() || id.slot >= m_motion_slots.size() ||
			id.idx >= m_motion_slots[id.slot].motion_defs()->size())
			return false;

		const CMotionDef* definition = m_motion_slots[id.slot].motion_def(id.idx);
		return definition && mark_index < definition->marks.size() &&
			definition->marks[mark_index].is_mark_between(time_previous, time_current);
	}

	bool configure_motion_playback_state(SMotionPlaybackState& state) override
	{
		if (!state.id.valid() || state.id.slot >= m_motion_slots.size() ||
			state.id.idx >= m_motion_slots[state.id.slot].motion_defs()->size())
			return false;

		const CMotion* root_motion = m_motion_slots[state.id.slot].bone_motion(m_bones[m_root_bone_id]->name,
			state.id.idx);
		if (!root_motion || root_motion->get_count() == 0)
			return false;

		CMotionDef* definition = m_motion_slots[state.id.slot].motion_def(state.id.idx);
		state.time_current = 0.f;
		state.time_total = float(root_motion->get_count()) * SAMPLE_SPF;
		state.speed = definition->Speed();
		state.weight = 1.f;
		state.blend_accrue = definition->Accrue();
		state.blend_falloff = definition->Falloff();
		state.blend_state = eMotionPlaybackBlendFixed;
		state.playing = true;
		state.stop_at_end = definition->StopAtEnd();
		state.callback_enabled = true;
		return std::isfinite(state.time_total) && state.time_total > 0.f && std::isfinite(state.speed);
	}

	bool get_motion_playback_states(SMotionPlaybackState* states, int capacity, int& count) const override
	{
		count = static_cast<int>(m_playback_states.size());
		if (capacity < count || (count && !states))
			return false;
		std::copy(m_playback_states.begin(), m_playback_states.end(), states);
		return true;
	}

	bool set_motion_playback_states(const SMotionPlaybackState* states, int count) override
	{
		if (count < 0 || count > static_cast<int>(MAX_CHANNELS * MAX_BLENDED) || (count && !states))
			return false;

		xr_vector<SMotionPlaybackState> replacement;
		replacement.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			const SMotionPlaybackState& state = states[i];
			if (!state.id.valid() || state.id.slot >= m_motion_slots.size() || state.channel >= MAX_CHANNELS ||
				state.blend_state > eMotionPlaybackBlendFalloff ||
				state.id.idx >= m_motion_slots[state.id.slot].motion_defs()->size() ||
				(state.partition_mask & ~((1u << MAX_PARTS) - 1)) ||
				!std::isfinite(state.time_current) || state.time_current < 0.f ||
				!std::isfinite(state.time_total) || state.time_total <= 0.f ||
				!std::isfinite(state.speed) || !std::isfinite(state.weight) || state.weight < 0.f ||
				!std::isfinite(state.blend_accrue) || state.blend_accrue < 0.f ||
				!std::isfinite(state.blend_falloff) || state.blend_falloff < 0.f)
				return false;
			if (!state.partition_mask &&
				m_motion_slots[state.id.slot].motion_def(state.id.idx)->bone_or_part >= MAX_PARTS)
				return false;
			replacement.push_back(state);
		}

		m_playback_states.swap(replacement);
		if (CalculateAnimatedPose())
			return true;
		m_playback_states.swap(replacement);
		return false;
	}

	bool advance_motion_playback(float dt) override
	{
		if (!std::isfinite(dt) || dt < 0.f)
			return false;

		bool changed = false;
		for (xr_vector<SMotionPlaybackState>::iterator it = m_playback_states.begin(); it != m_playback_states.end();)
		{
			SMotionPlaybackState& state = *it;
			if (state.blend_state == eMotionPlaybackBlendAccrue)
			{
				state.weight += dt * state.blend_accrue * state.speed;
				if (state.weight >= 1.f)
				{
					state.weight = 1.f;
					state.blend_state = eMotionPlaybackBlendFixed;
				}
				changed = true;
			}
			else if (state.blend_state == eMotionPlaybackBlendFalloff)
			{
				state.weight -= dt * state.blend_falloff * state.speed;
				if (state.weight <= EPS_S)
				{
					it = m_playback_states.erase(it);
					changed = true;
					continue;
				}
				changed = true;
			}

			if (state.playing && state.advance_time(dt))
			{
				state.playing = false;
				changed = true;
			}
			++it;
		}
		return changed && CalculateAnimatedPose();
	}
};

// Static OGF models have bounds but no skeletal pose. Dedicated-server cform
// still needs a CPU collision provider, so expose a conservative bounds box as
// one synthetic bone. Client rendering and authored skeletal cforms are not
// changed by this fallback.
class CCpuBoundsCollisionPose : public IObjectCollisionPose
{
	Fbox m_bounds_box;
	Fsphere m_bounds_sphere;
	SBoneShape m_shape;
	Fmatrix m_identity;

public:
	static CCpuBoundsCollisionPose* Create(const ogf_header& header)
	{
		const Fvector& min = header.bb.min;
		const Fvector& max = header.bb.max;
		if (!std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(min.z) ||
			!std::isfinite(max.x) || !std::isfinite(max.y) || !std::isfinite(max.z) ||
			max.x <= min.x || max.y <= min.y || max.z <= min.z)
			return nullptr;

		CCpuBoundsCollisionPose* pose = xr_new<CCpuBoundsCollisionPose>();
		pose->m_bounds_box.set(min, max);
		pose->m_bounds_box.getsphere(pose->m_bounds_sphere.P, pose->m_bounds_sphere.R);

		Fvector center;
		Fvector half_size;
		pose->m_bounds_box.getcenter(center);
		pose->m_bounds_box.getradius(half_size);
		Fmatrix box_transform;
		box_transform.identity();
		box_transform.c.set(center);
		pose->m_shape.type = SBoneShape::stBox;
		pose->m_shape.box.xform_set(box_transform);
		pose->m_shape.box.m_halfsize.set(half_size);
		pose->m_identity.identity();
		return pose;
	}

	const Fbox& bounds_box() const override { return m_bounds_box; }
	const Fsphere& bounds_sphere() const override { return m_bounds_sphere; }
	void calculate_pose() override {}
	u64 visible_bones() override { return 1; }
	u16 bone_count() const override { return 1; }
	BOOL bone_visible(u16 bone_id) override { return bone_id == 0; }
	const SBoneShape& bone_shape(u16 bone_id) override { return m_shape; }
	const Fmatrix& bone_transform(u16 bone_id) override { return m_identity; }
};

bool ResolveOGFPath(LPCSTR model_name, string_path& path)
{
	if (!model_name || !model_name[0])
		return false;

	string_path ogf_name;
	if (strext(model_name))
		xr_strcpy(ogf_name, sizeof(ogf_name), model_name);
	else
		strconcat(sizeof(ogf_name), ogf_name, model_name, ".ogf");

	if (FS.exist(path, "$level$", ogf_name) || FS.exist(path, "$game_meshes$", ogf_name))
		return true;
	if (!FS.exist(ogf_name))
		return false;
	xr_strcpy(path, sizeof(string_path), ogf_name);
	return true;
}
}

IObjectCollisionPose* CreateRigidCollisionPoseFromOGF(LPCSTR model_name)
{
	string_path ogf_path;
	if (!ResolveOGFPath(model_name, ogf_path))
		return nullptr;

	IReader* data = FS.r_open(ogf_path);
	if (!data)
		return nullptr;
	ogf_header header;
	data->r_chunk_safe(OGF_HEADER, &header, sizeof(header));
	IObjectCollisionPose* pose = header.type == MT_SKELETON_RIGID ? CCpuRigidCollisionPose::Create(*data) : nullptr;
	FS.r_close(data);
	return pose;
}

IObjectCollisionPose* CreateCpuCollisionPoseFromOGF(LPCSTR model_name, CObjectList* object_list)
{
	string_path ogf_path;
	if (!ResolveOGFPath(model_name, ogf_path))
		return nullptr;

	IReader* data = FS.r_open(ogf_path);
	if (!data)
		return nullptr;
	ogf_header header;
	data->r_chunk_safe(OGF_HEADER, &header, sizeof(header));

	IObjectCollisionPose* pose = nullptr;
	if (header.type == MT_SKELETON_RIGID)
		pose = CCpuRigidCollisionPose::Create(*data);
	else if (header.type == MT_SKELETON_ANIM)
	{
		if (object_list)
			pose = CCpuAnimatedCollisionPose::Create(*data, model_name, &object_list->CpuMotionsCache());
		if (!pose)
			Msg("! CPU animated collision pose could not load OGF/OMF assets for '%s'.", model_name);
	}

	// Rigid vehicles and static props may carry OGF skeleton tags but no usable
	// IK/bone chunks. Preserve the authored CPU pose when it parses; otherwise
	// dedicated mode can still collide against the validated model bounds. Never
	// apply this approximation to animated actors, whose pose drives gameplay.
	if (!pose && header.type != MT_SKELETON_ANIM)
	{
		pose = CCpuBoundsCollisionPose::Create(header);
		if (pose)
			Msg("! Using conservative OGF bounds-box collision fallback for '%s' (type=%u, rigid-pose=%s).",
				model_name, u32(header.type), header.type == MT_SKELETON_RIGID ? "unavailable" : "not applicable");
	}

	FS.r_close(data);
	return pose;
}
