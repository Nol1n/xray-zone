//---------------------------------------------------------------------------
#ifndef SkeletonMotionsH
#define SkeletonMotionsH

//#include "skeletoncustom.h"
#include "bone.h"
#include "skeletonmotiondefs.h"
#include "SkeletonMotionId.h"
// refs
class CKinematicsAnimated;
class CBlend;
class IKinematics;

// callback
typedef void (*PlayCallback)(CBlend* P);


//*** Key frame definition ************************************************************************
enum
{
	flTKeyPresent = (1 << 0),
	flRKeyAbsent = (1 << 1),
	flTKey16IsBit = (1 << 2),
};
#pragma pack(push,2)
struct CKey
{
	Fquaternion Q; // rotation
	Fvector T; // translation
};

struct CKeyQR
{
	s16 x, y, z, w; // rotation
};

struct CKeyQT8
{
	s8 x1, y1, z1;
};

struct CKeyQT16
{
	s16 x1, y1, z1;
};

/*
struct CKeyQT
{
// s8 x,y,z;
s16 x1,y1,z1;
};
*/
#pragma pack(pop)

//*** Motion Data *********************************************************************************
class ENGINE_API CMotion
{
	struct
	{
		u32 _flags : 8;
		u32 _count : 24;
	};

public:
	ref_smem<CKeyQR> _keysR;
	ref_smem<CKeyQT8> _keysT8;
	ref_smem<CKeyQT16> _keysT16;
	Fvector _initT;
	Fvector _sizeT;
public:
	void set_flags(u8 val) { _flags = val; }

	void set_flag(u8 mask, u8 val)
	{
		if (val)_flags |= mask;
		else _flags &= ~mask;
	}

	BOOL test_flag(u8 mask) const { return BOOL(_flags & mask); }

	void set_count(u32 cnt)
	{
		VERIFY(cnt);
		_count = cnt;
	}

	ICF u32 get_count() const { return (u32(_count) & 0x00FFFFFF); }

	float GetLength() { return float(_count) * SAMPLE_SPF; }

	u32 mem_usage()
	{
		u32 sz = sizeof(*this);
		if (_keysR.size()) sz += _keysR.size() * sizeof(CKeyQR) / _keysR.ref_count();
		if (_keysT8.size()) sz += _keysT8.size() * sizeof(CKeyQT8) / _keysT8.ref_count();
		if (_keysT16.size()) sz += _keysT16.size() * sizeof(CKeyQT16) / _keysT16.ref_count();
		return sz;
	}
};

// Advance a cycle clock without depending on CBlend, RDEVICE or renderer state.
IC bool AdvanceMotionPlaybackTime(float& time_current, float time_total, float speed, bool playing, bool stop_at_end, float dt)
{
	if (!playing)
		return false;
	float quant = dt * speed;
	time_current += quant;
	const bool running_forward = quant > 0.f;
	const float end_epsilon = SAMPLE_SPF + EPS;
	const bool at_end = running_forward && (time_current > (time_total - end_epsilon));
	const bool at_begin = !running_forward && (time_current < 0.f);

	if (!stop_at_end)
	{
		if (at_begin)
			time_current += time_total;
		if (at_end)
			time_current -= (time_total - end_epsilon);
		VERIFY(time_current >= 0.f);
		return false;
	}
	if (!at_end && !at_begin)
		return false;

	if (at_end)
	{
		time_current = time_total - end_epsilon;
		if (time_current < 0.f)
			time_current = 0.f;
	}
	else
		time_current = 0.f;

	VERIFY(time_current >= 0.f);
	return true;
}

// Caller-owned CPU state for one selected cycle. Blend transitions and
// completion callbacks remain responsibilities of the playback controller.
enum EMotionPlaybackBlendState : u8
{
	eMotionPlaybackBlendFixed,
	eMotionPlaybackBlendAccrue,
	eMotionPlaybackBlendFalloff,
};

struct SMotionPlaybackState
{
	MotionID id;
	u8 channel;
	u8 controller_group;
	u8 blend_state;
	// Zero uses the motion definition's partition. A nonzero mask lets global
	// playback target several rig partitions, as the renderer does.
	u32 partition_mask;
	float time_current;
	float time_total;
	float speed;
	float weight;
	float blend_accrue;
	float blend_falloff;
	bool playing;
	bool stop_at_end;
	bool callback_enabled;

	SMotionPlaybackState()
		: channel(0), controller_group(0), blend_state(eMotionPlaybackBlendFixed), partition_mask(0), time_current(0.f),
		  time_total(0.f), speed(1.f), weight(1.f), blend_accrue(0.f), blend_falloff(0.f), playing(false),
		  stop_at_end(false), callback_enabled(true)
	{}

	bool advance_time(float dt)
	{
		return AdvanceMotionPlaybackTime(time_current, time_total, speed, playing, stop_at_end, dt);
	}
};

// Sample one compressed bone track at a caller-owned time. This is deliberately
// independent of CBlend, RDEVICE and renderer state so simulation can reuse it.
IC void EvaluateMotionKey(CKey& result, float time_seconds, const CMotion& motion)
{
	const float sample_time = time_seconds * float(SAMPLE_FPS);
	VERIFY(sample_time >= 0.f);
	const u32 frame = iFloor(sample_time);
	const float delta = sample_time - float(frame);
	const u32 count = motion.get_count();

	if (motion.test_flag(flRKeyAbsent))
	{
		const CKeyQR& key = motion._keysR[0];
		result.Q.set(float(key.x) * KEY_QuantI,
		             float(key.y) * KEY_QuantI,
		             float(key.z) * KEY_QuantI,
		             float(key.w) * KEY_QuantI);
	}
	else
	{
		const CKeyQR& key1 = motion._keysR[(frame + 0) % count];
		const CKeyQR& key2 = motion._keysR[(frame + 1) % count];
		Fquaternion q1, q2;
		q1.set(float(key1.x) * KEY_QuantI,
		       float(key1.y) * KEY_QuantI,
		       float(key1.z) * KEY_QuantI,
		       float(key1.w) * KEY_QuantI);
		q2.set(float(key2.x) * KEY_QuantI,
		       float(key2.y) * KEY_QuantI,
		       float(key2.z) * KEY_QuantI,
		       float(key2.w) * KEY_QuantI);
		result.Q.slerp(q1, q2, clampr(delta, 0.f, 1.f));
	}

	if (!motion.test_flag(flTKeyPresent))
	{
		result.T.set(motion._initT);
		return;
	}

	Fvector translation1, translation2;
	if (motion.test_flag(flTKey16IsBit))
	{
		const CKeyQT16& key1 = motion._keysT16[(frame + 0) % count];
		const CKeyQT16& key2 = motion._keysT16[(frame + 1) % count];
		translation1.set(float(key1.x1) * motion._sizeT.x + motion._initT.x,
		                 float(key1.y1) * motion._sizeT.y + motion._initT.y,
		                 float(key1.z1) * motion._sizeT.z + motion._initT.z);
		translation2.set(float(key2.x1) * motion._sizeT.x + motion._initT.x,
		                 float(key2.y1) * motion._sizeT.y + motion._initT.y,
		                 float(key2.z1) * motion._sizeT.z + motion._initT.z);
	}
	else
	{
		const CKeyQT8& key1 = motion._keysT8[(frame + 0) % count];
		const CKeyQT8& key2 = motion._keysT8[(frame + 1) % count];
		translation1.set(float(key1.x1) * motion._sizeT.x + motion._initT.x,
		                 float(key1.y1) * motion._sizeT.y + motion._initT.y,
		                 float(key1.z1) * motion._sizeT.z + motion._initT.z);
		translation2.set(float(key2.x1) * motion._sizeT.x + motion._initT.x,
		                 float(key2.y1) * motion._sizeT.y + motion._initT.y,
		                 float(key2.z1) * motion._sizeT.z + motion._initT.z);
	}
	result.T.lerp(translation1, translation2, delta);
}

// Compose the local animated transform under its parent. Renderer and CPU
// collision poses must use the same row-vector matrix order.
IC void ComposeMotionBoneTransform(Fmatrix& result, const Fmatrix& parent, const Fmatrix& local)
{
	result.mul_43(parent, local);
}

IC void ComposeMotionBoneTransform(Fmatrix& result, const Fmatrix& parent, const CKey& key)
{
	Fmatrix local;
	local.mk_xform(key.Q, key.T);
	ComposeMotionBoneTransform(result, parent, local);
}

struct SMotionBlendKey
{
	const CKey* key;
	float weight;

	bool operator<(const SMotionBlendKey& other) const { return weight > other.weight; }
};

// Blend evaluated per-bone cycle keys using the same weighted interpolation as
// the renderer's MixInterlerp path.
IC void BlendMotionKeys(CKey& result, const CKey* keys, const float* weights, int count)
{
	VERIFY(count >= 0 && count <= static_cast<int>(MAX_BLENDED));
	if (count == 0)
	{
		result.Q.set(0.f, 0.f, 0.f, 0.f);
		result.T.set(0.f, 0.f, 0.f);
		return;
	}
	if (count == 1)
	{
		result = keys[0];
		return;
	}
	if (count == 2)
	{
		const float weight_sum = weights[0] + weights[1];
		const float delta = fis_zero(weight_sum) ? 0.f : weights[1] / weight_sum;
		const float blend = clampr(delta, 0.f, 1.f);
		result.Q.slerp(keys[0].Q, keys[1].Q, blend);
		result.T.lerp(keys[0].T, keys[1].T, blend);
		return;
	}

	SMotionBlendKey ordered[MAX_BLENDED];
	for (int i = 0; i < count; ++i)
	{
		ordered[i].key = keys + i;
		ordered[i].weight = weights[i];
	}
	std::sort(ordered, ordered + count);

	float total = ordered[0].weight;
	result = *ordered[0].key;
	for (int i = 1; i < count; ++i)
	{
		total += ordered[i].weight;
		const float delta = fis_zero(total) ? 0.f : ordered[i].weight / total;
		const float blend = clampr(delta, 0.f, 1.f);
		CKey mixed;
		mixed.Q.slerp(result.Q, ordered[i].key->Q, blend);
		mixed.T.lerp(result.T, ordered[i].key->T, blend);
		result = mixed;
	}
}

enum EMotionChannelMix : u8
{
	eMotionChannelLerp,
	eMotionChannelAdd,
};

struct SMotionChannelDef
{
	float factor;
	EMotionChannelMix mix;
};

// Combine channel results in source order. Additive channels follow the same
// quaternion and translation operation as the current renderer implementation.
IC void MixMotionChannels(CKey& result, const CKey* keys, const SMotionChannelDef* channels, int count)
{
	VERIFY(count > 0);
	result = keys[0];
	float lerp_factor_sum = 0.f;
	for (int i = 1; i < count; ++i)
	{
		if (channels[i].mix == eMotionChannelAdd)
		{
			Fquaternion scaled_rotation = keys[i].Q;
			float angle;
			Fvector axis;
			scaled_rotation.get_axis_angle(axis, angle);
			scaled_rotation.rotation(axis, angle * channels[i].factor);

			const Fquaternion base_rotation = result.Q;
			Fquaternion combined_rotation;
			combined_rotation.mul(base_rotation, scaled_rotation);
			result.Q.set(combined_rotation);

			Fvector scaled_translation = keys[i].T;
			scaled_translation.mul(channels[i].factor);
			const Fvector base_translation = result.T;
			result.T.add(base_translation, scaled_translation);
		}
		else if (channels[i].mix == eMotionChannelLerp)
		{
			lerp_factor_sum += channels[i].factor;
			const float delta = channels[i].factor / lerp_factor_sum;
			const CKey base_key = result;
			result.Q.slerp(base_key.Q, keys[i].Q, delta);
			result.T.lerp(base_key.T, keys[i].T, delta);
		}
		else
			NODEFAULT;
	}
}

struct SMotionBlendTrack
{
	const CMotion* motion;
	float time_seconds;
	float weight;
};

struct SMotionChannelTrackSet
{
	const SMotionBlendTrack* tracks;
	int count;
	SMotionChannelDef definition;
};

// Evaluate one bone from caller-selected tracks. Additive tracks are made
// relative to their first key before per-channel blending, matching the renderer.
IC void EvaluateMotionBone(CKey& result, const SMotionChannelTrackSet* channels, int channel_count)
{
	VERIFY(channel_count > 0 && channel_count <= static_cast<int>(MAX_CHANNELS));
	CKey channel_keys[MAX_CHANNELS];
	SMotionChannelDef active_channels[MAX_CHANNELS];
	int active_count = 0;

	for (int channel_index = 0; channel_index < channel_count; ++channel_index)
	{
		const SMotionChannelTrackSet& channel = channels[channel_index];
		VERIFY(channel.count >= 0 && channel.count <= static_cast<int>(MAX_BLENDED));
		if (channel_index != 0 && channel.count == 0)
			continue;

		CKey track_keys[MAX_BLENDED];
		float weights[MAX_BLENDED];
		for (int track_index = 0; track_index < channel.count; ++track_index)
		{
			const SMotionBlendTrack& track = channel.tracks[track_index];
			VERIFY(track.motion);
			EvaluateMotionKey(track_keys[track_index], track.time_seconds, *track.motion);
			if (channel.definition.mix == eMotionChannelAdd)
			{
				CKey reference_key;
				EvaluateMotionKey(reference_key, 0.f, *track.motion);
				Fquaternion inverse_reference;
				inverse_reference.inverse(reference_key.Q);
				Fquaternion relative_rotation;
				relative_rotation.mul(track_keys[track_index].Q, inverse_reference);
				track_keys[track_index].Q.set(relative_rotation);
				Fvector relative_translation;
				relative_translation.sub(track_keys[track_index].T, reference_key.T);
				track_keys[track_index].T.set(relative_translation);
			}
			weights[track_index] = track.weight;
		}

		BlendMotionKeys(channel_keys[active_count], track_keys, weights, channel.count);
		active_channels[active_count] = channel.definition;
		++active_count;
	}

	MixMotionChannels(result, channel_keys, active_channels, active_count);
}

class ENGINE_API motion_marks
{
public:
	typedef std::pair<float, float> interval;
#ifdef _EDITOR
public:
#else
private:
#endif
	typedef xr_vector<interval> STORAGE;
	typedef STORAGE::iterator ITERATOR;
	typedef STORAGE::const_iterator C_ITERATOR;

	STORAGE intervals;
public:
	shared_str name;
	void Load(IReader*);

#ifdef _EDITOR
    void Save(IWriter*);
#endif
	bool is_empty() const { return intervals.empty(); }
	const interval* pick_mark(float const& t) const;
	bool is_mark_between(float const& t0, float const& t1) const;
	float time_to_next_mark(float time) const;
};


const float fQuantizerRangeExt = 1.5f;

class ENGINE_API CMotionDef
{
public:
	u16 bone_or_part;
	u16 motion;
	u16 speed; // quantized: 0..10
	u16 power; // quantized: 0..10
	u16 accrue; // quantized: 0..10
	u16 falloff; // quantized: 0..10
	u16 flags;
	xr_vector<motion_marks> marks;

	IC float Dequantize(u16 V) const { return float(V) / 655.35f; }
	IC u16 Quantize(float V) const
	{
		s32 t = iFloor(V * 655.35f);
		clamp(t, 0, 65535);
		return u16(t);
	}

	void Load(IReader* MP, u32 fl, u16 vers);
	u32 mem_usage() { return sizeof(*this); }

	ICF float Accrue() { return fQuantizerRangeExt * Dequantize(accrue); }
	ICF float Falloff() { return fQuantizerRangeExt * Dequantize(falloff); }
	ICF float Speed() { return Dequantize(speed); }
	ICF float Power() { return Dequantize(power); }
	bool StopAtEnd();
};

struct accel_str_pred
{
	IC bool operator()(const shared_str& x, const shared_str& y) const { return xr_strcmp(x, y) < 0; }
};

typedef xr_map<shared_str, u16, accel_str_pred> accel_map;
DEFINE_VECTOR(CMotionDef, MotionDefVec, MotionDefVecIt);

DEFINE_VECTOR(CMotion, MotionVec, MotionVecIt);
DEFINE_VECTOR(MotionVec*, BoneMotionsVec, BoneMotionsVecIt);
DEFINE_MAP(shared_str, MotionVec, BoneMotionMap, BoneMotionMapIt);

// partition
class ENGINE_API CPartDef
{
public:
	shared_str Name;
	xr_vector<u32> bones;

	CPartDef() : Name(0)
	{
	};

	u32 mem_usage() { return sizeof(*this) + bones.size() * sizeof(u32) + sizeof(Name); }
};

class ENGINE_API CPartition
{
	xr_vector<CPartDef*> P;
public:
	IC CPartDef* operator[](u16 id) { return P.size() > id ? P.at(id) : nullptr; }
	IC const CPartDef* part(u16 id) const { return P.size() > id ? P.at(id) : nullptr; }
	CPartDef* create()
	{
		if (P.size() > MAX_PARTS) return nullptr;
		P.emplace_back(xr_new<CPartDef>());
		return P.back();
	}
	u16 part_id(const shared_str& name) const;
	u32 mem_usage() { return P[0]->mem_usage() * P.size(); }
	void load(IKinematics* V, LPCSTR model_name);

	u8 count() const { return P.size(); };

	CPartition()
	{
		P.reserve(MAX_PARTS);
	}

	~CPartition()
	{
		for (auto& PartDef : P)
			xr_delete(PartDef);
	}
};

// shared motions
struct ENGINE_API motions_value
{
	accel_map m_motion_map; // motion associations
	accel_map m_cycle; // motion data itself (shared)
	accel_map m_fx; // motion data itself (shared)
	CPartition m_partition; // partition
	u32 m_dwReference;
	BoneMotionMap m_motions;
	MotionDefVec m_mdefs;

	shared_str m_id;


	BOOL load(LPCSTR N, IReader* data, vecBones* bones);
	MotionVec* bone_motions(shared_str bone_name);

	u32 mem_usage()
	{
		u32 sz = sizeof(*this) + m_motion_map.size() * 6 + m_partition.mem_usage();
		for (MotionDefVecIt it = m_mdefs.begin(); it != m_mdefs.end(); ++it)
			sz += it->mem_usage();
		for (BoneMotionMapIt bm_it = m_motions.begin(); bm_it != m_motions.end(); ++bm_it)
			for (MotionVecIt m_it = bm_it->second.begin(); m_it != bm_it->second.end(); ++m_it)
				sz += m_it->mem_usage();
		return sz;
	}
};

class ENGINE_API motions_container
{
	DEFINE_MAP(shared_str, motions_value*, SharedMotionsMap, SharedMotionsMapIt);
	SharedMotionsMap container;
public:
	motions_container();
	~motions_container();
	bool has(shared_str key);
	motions_value* dock(shared_str key, IReader* data, vecBones* bones);
	// cache_key identifies the rig-compatible cache entry; motion_name is the
	// actual OMF name recorded in the loaded motions_value.
	motions_value* dock(shared_str cache_key, shared_str motion_name, IReader* data, vecBones* bones);
	void dump();
	void clean(bool force_destroy);
	bool empty() const { return container.empty(); }
};

// The same OMF can be shared only when the ordered rig bone names match.
ENGINE_API shared_str make_motion_cache_key(shared_str motion_name, const vecBones& bones);

extern ENGINE_API motions_container* g_pMotionsContainer;

class ENGINE_API shared_motions
{
private:
	motions_value* p_;
protected:
	// ref-counting
	void destroy()
	{
		if (0 == p_) return;
		p_->m_dwReference--;
		if (0 == p_->m_dwReference) p_ = 0;
	}

public:
	bool create(shared_str key, IReader* data, vecBones* bones);
	bool create(shared_str key, IReader* data, vecBones* bones, motions_container& cache);
	bool create(shared_str cache_key, shared_str motion_name, IReader* data, vecBones* bones, motions_container& cache);
	bool create_from_vfs(shared_str motion_name, vecBones* bones, motions_container& cache);
	//{ motions_value* v = g_pMotionsContainer->dock(key,data,bones); if (0!=v) v->m_dwReference++; destroy(); p_ = v; }
	bool create(shared_motions const& rhs);
	// { motions_value* v = rhs.p_; if (0!=v) v->m_dwReference++; destroy(); p_ = v; }
public:
	// construction
	shared_motions() { p_ = 0; }

	shared_motions(shared_motions const& rhs)
	{
		p_ = 0;
		create(rhs);
	}

	~shared_motions() { destroy(); }

	// assignment & accessors
	shared_motions& operator=(shared_motions const& rhs)
	{
		create(rhs);
		return *this;
	}

	bool operator==(shared_motions const& rhs) const { return (p_ == rhs.p_); }

	// misc func
	MotionVec* bone_motions(const shared_str& bone_name)
	{
		VERIFY(p_);
		return p_->bone_motions(bone_name);
	}

	accel_map* motion_map()
	{
		VERIFY(p_);
		return &p_->m_motion_map;
	}

	accel_map* cycle()
	{
		VERIFY(p_);
		return &p_->m_cycle;
	}

	accel_map* fx()
	{
		VERIFY(p_);
		return &p_->m_fx;
	}

	CPartition* partition()
	{
		VERIFY(p_);
		return &p_->m_partition;
	}

	MotionDefVec* motion_defs()
	{
		VERIFY(p_);
		return &p_->m_mdefs;
	}

	const MotionDefVec* motion_defs() const
	{
		VERIFY(p_);
		return &p_->m_mdefs;
	}

	const CMotion* bone_motion(const shared_str& bone_name, u16 motion_index) const;

	CMotionDef* motion_def(u16 idx)
	{
		VERIFY(p_);
		return &p_->m_mdefs[idx];
	}

	const CMotionDef* motion_def(u16 idx) const
	{
		VERIFY(p_);
		return &p_->m_mdefs[idx];
	}

	const shared_str& id() const
	{
		VERIFY(p_);
		return p_->m_id;
	}
};

// Resolve a selected CPU playback state to the compressed per-bone track in
// its loaded OMF slot. The output pointer remains owned by `slots`.
ENGINE_API bool ResolveMotionPlaybackTrack(const shared_motions* slots, u16 slot_count, const shared_str& bone_name,
                                          const SMotionPlaybackState& state, SMotionBlendTrack& track);
ENGINE_API bool EvaluateMotionBoneFromStates(CKey& result, const shared_motions* slots, u16 slot_count,
	const shared_str& bone_name, const SMotionPlaybackState* states, int state_count,
	const SMotionChannelDef* channel_definitions, int channel_count);

//---------------------------------------------------------------------------
#endif
