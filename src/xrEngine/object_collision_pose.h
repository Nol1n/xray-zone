#pragma once

struct SBoneShape;
struct MotionID;
struct SMotionPlaybackState;

// CPU contract needed by dynamic object bounds and skeletal collision.
// Implementations may be supplied by a model provider or owned by the object.
class IObjectCollisionPose
{
public:
	virtual ~IObjectCollisionPose() = default;

	virtual const Fbox& bounds_box() const = 0;
	virtual const Fsphere& bounds_sphere() const = 0;
	virtual void calculate_pose() = 0;
	virtual u64 visible_bones() = 0;
	virtual u16 bone_count() const = 0;
	virtual BOOL bone_visible(u16 bone_id) = 0;
	virtual const SBoneShape& bone_shape(u16 bone_id) = 0;
	virtual const Fmatrix& bone_transform(u16 bone_id) = 0;
	// Resolve authored gameplay bone names without requiring a render skeleton.
	virtual u16 bone_id(LPCSTR) const { return u16(-1); }
	// Animated CPU providers may accept gameplay-selected cycles. Other pose
	// sources keep the default implementation and remain renderer-owned.
	virtual bool find_cycle(LPCSTR, MotionID&) { return false; }
	virtual bool find_fx(LPCSTR, MotionID&) { return false; }
	virtual bool motion_partition(const MotionID&, u16&) const { return false; }
	virtual bool motion_uses_root_mover(const MotionID&) const { return false; }
	// Sample one animation state at full weight on the renderer's base channel.
	// This is side-effect free; callers choose when to remove root motion from collision.
	virtual bool sample_root_motion_transform(const SMotionPlaybackState&, Fmatrix&) const { return false; }
	virtual bool set_root_motion_extraction(bool) { return false; }
	virtual bool motion_mark_between(const MotionID&, u16, float, float) const { return false; }
	virtual bool configure_motion_playback_state(SMotionPlaybackState&) { return false; }
	virtual bool get_motion_playback_states(SMotionPlaybackState*, int, int&) const { return false; }
	virtual bool set_motion_playback_states(const SMotionPlaybackState*, int) { return false; }
	virtual bool advance_motion_playback(float dt) { return false; }
};
