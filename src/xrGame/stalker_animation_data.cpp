////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_data.cpp
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation data
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_animation_data.h"

CStalkerAnimationData::CStalkerAnimationData(IKinematicsAnimated* skeleton_animated)
{
	m_part_animations.Load(skeleton_animated, "");
	m_head_animations.Load(skeleton_animated, "");
	m_global_animations.Load(skeleton_animated, "item_");
}

CStalkerAnimationData::CStalkerAnimationData(IObjectCollisionPose* pose)
{
	m_part_animations.Load(pose, "");
	m_head_animations.Load(pose, "");
	m_global_animations.Load(pose, "item_");
}
