#pragma once

#include "object_collision_pose.h"

class CObjectList;

// Returns a CPU-only pose provider for rigid skeletal OGF assets, or nullptr
// when the asset is missing, animated, or does not contain skeletal chunks.
IObjectCollisionPose* CreateRigidCollisionPoseFromOGF(LPCSTR model_name);

// Returns a CPU-only pose provider for rigid or animated skeletal OGF assets.
// Animated OMF references are acquired from the level-owned motion cache.
IObjectCollisionPose* CreateCpuCollisionPoseFromOGF(LPCSTR model_name, CObjectList* object_list);
