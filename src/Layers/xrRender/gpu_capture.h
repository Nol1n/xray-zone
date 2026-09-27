#pragma once

namespace zone_gpu
{
// Ordered boundaries on the immediate render context, only on sampled frames.
// Six adjacent regions partition the same timestamps as the whole-frame sample.
enum class Marker { SceneBegin, Prepared, GeometryEnd, LightingEnd, CombineEnd, Count };
void mark(Marker marker);
}
