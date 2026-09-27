////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_monsters_anims.h
//	Created 	: 23.05.2003
//  Modified 	: 23.05.2003
//	Author		: Serge Zhem
//	Description : Animation templates for all of the monsters
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "../../Include/xrRender/KinematicsAnimated.h"
#include "../../xrEngine/object_collision_pose.h"
#include "../ai_debug.h"

DEFINE_VECTOR(MotionID, ANIM_VECTOR, ANIM_IT);

class CAniVector
{
public:
	ANIM_VECTOR A;

	void Load(IKinematicsAnimated* tpKinematics, LPCSTR caBaseName);
	void Load(IObjectCollisionPose* pose, LPCSTR base_name)
	{
		A.clear();
		string256 name;
		string256 index;
		MotionID motion;
		for (int i = 0; ; ++i)
		{
			strconcat(sizeof(name), name, base_name, itoa(i, index, 10));
			if (pose->find_cycle(name, motion) || pose->find_fx(name, motion))
				A.push_back(motion);
			else if (i < 10)
				continue;
			else
				break;
		}
	}
};

template <LPCSTR caBaseNames[]>
class CAniFVector
{
public:
	ANIM_VECTOR A;

	IC void Load(IKinematicsAnimated* tpKinematics, LPCSTR caBaseName)
	{
		A.clear();
		string256 S;
		int j = 0;
		for (; caBaseNames[j]; ++j);
		A.resize(j);
		for (int i = 0; i < j; ++i)
		{
			strconcat(sizeof(S), S, caBaseName, caBaseNames[i]);
			A[i] = tpKinematics->ID_Cycle_Safe(S);
#ifdef DEBUG
			if (A[i] && psAI_Flags.test(aiAnimation))
				Msg		("* Loaded animation %s",S);
#endif
		}
	}

	IC void Load(IObjectCollisionPose* pose, LPCSTR base_name)
	{
		A.clear();
		string256 name;
		int count = 0;
		for (; caBaseNames[count]; ++count);
		A.resize(count);
		for (int i = 0; i < count; ++i)
		{
			strconcat(sizeof(name), name, base_name, caBaseNames[i]);
			A[i].invalidate();
			pose->find_cycle(name, A[i]);
		}
	}
};

template <class TYPE_NAME, LPCSTR caBaseNames[]>
class CAniCollection
{
public:
	xr_vector<TYPE_NAME> A;

	IC void Load(IKinematicsAnimated* tpKinematics, LPCSTR caBaseName)
	{
		A.clear();
		string256 S;
		int j = 0;
		for (; caBaseNames[j]; ++j);
		A.resize(j);
		for (int i = 0; i < j; ++i)
			A[i].Load(tpKinematics, strconcat(sizeof(S), S, caBaseName, caBaseNames[i]));
	}

	IC void Load(IObjectCollisionPose* pose, LPCSTR base_name)
	{
		A.clear();
		string256 name;
		int count = 0;
		for (; caBaseNames[count]; ++count);
		A.resize(count);
		for (int i = 0; i < count; ++i)
			A[i].Load(pose, strconcat(sizeof(name), name, base_name, caBaseNames[i]));
	}
};
