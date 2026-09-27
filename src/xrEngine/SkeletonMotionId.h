#pragma once

// A motion definition address is an asset/runtime identifier shared by CPU and
// renderer playback. Keep its original packed representation for compatibility.
struct MotionID
{
private:
	typedef const MotionID* (MotionID::*unspecified_bool_type)() const;

public:
	union
	{
		struct
		{
			u16 idx : 16;
			u16 slot : 16;
		};
		u32 val;
	};

	MotionID() { invalidate(); }
	MotionID(u16 motion_slot, u16 motion_idx) { set(motion_slot, motion_idx); }
	ICF bool operator==(const MotionID& target) const { return target.val == val; }
	ICF bool operator!=(const MotionID& target) const { return target.val != val; }
	ICF bool operator<(const MotionID& target) const { return val < target.val; }
	ICF bool operator!() const { return !valid(); }
	ICF void set(u16 motion_slot, u16 motion_idx)
	{
		slot = motion_slot;
		idx = motion_idx;
	}
	ICF void invalidate() { val = u16(-1); }
	ICF bool valid() const { return val != u16(-1); }
	const MotionID* get() const { return this; }
	ICF operator unspecified_bool_type() const
	{
		return valid() ? &MotionID::get : 0;
	}
};
