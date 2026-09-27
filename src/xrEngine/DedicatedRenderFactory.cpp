#include "stdafx.h"

#include "../Layers/xrRender/dxRenderFactory.h"
#include "../Layers/xrRender/xrRender_console.h"

#ifdef DEDICATED_SERVER

// The dedicated build keeps the shared engine/game interfaces but provides no
// graphics implementation. Gameplay-side callers must not dereference objects
// returned by this factory; server/client lifecycle separation guards those
// paths before the server is started.
dxRenderFactory RenderFactoryImpl;

#define DEDICATED_RENDER_FACTORY_IMPLEMENT(Class) \
	I##Class* dxRenderFactory::Create##Class() { return nullptr; } \
	void dxRenderFactory::Destroy##Class(I##Class*) {}

#ifndef _EDITOR
DEDICATED_RENDER_FACTORY_IMPLEMENT(UISequenceVideoItem)
DEDICATED_RENDER_FACTORY_IMPLEMENT(UIShader)
DEDICATED_RENDER_FACTORY_IMPLEMENT(StatGraphRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(ConsoleRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(RenderDeviceRender)
#	ifdef DEBUG
DEDICATED_RENDER_FACTORY_IMPLEMENT(ObjectSpaceRender)
#	endif
DEDICATED_RENDER_FACTORY_IMPLEMENT(ApplicationRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(WallMarkArray)
DEDICATED_RENDER_FACTORY_IMPLEMENT(StatsRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(FlareRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(ThunderboltRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(ThunderboltDescRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(RainRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(LensFlareRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(ImGuiRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(EnvironmentRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(EnvDescriptorMixerRender)
DEDICATED_RENDER_FACTORY_IMPLEMENT(EnvDescriptorRender)
#endif
DEDICATED_RENDER_FACTORY_IMPLEMENT(FontRender)

// These settings are shared with gameplay code but normally live in the
// renderer module. Preserve gameplay-affecting defaults without linking it.
Fvector4 ps_ssfx_wind_trees = { 11.0f, 0.15f, 0.5f, 0.15f };
Fvector4 ps_ssfx_grass_interactive = { 0.0f, 0.0f, 2000.0f, 1.0f };
Fvector4 ps_ssfx_int_grass_params_1 = { 1.0f, 1.0f, 1.0f, 25.0f };
Fvector4 ps_ssfx_int_grass_params_2 = { 1.0f, 5.0f, 1.0f, 1.0f };

Flags32 psDeviceFlags2 = { 0 };
Flags32 ps_actor_shadow_flags = { 0 };
int ps_r4_hdr10_pda = 0;
BOOL r_optimize_calculate_bones = TRUE;
float wallmark_range_static = 100.0f;
float wallmark_range_skeleton = 50.0f;
float hud_fov_aim_factor = 0.0f;

float sil_glow_max_temp = 0.15f;
float sil_glow_shot_temp = 0.004f;
float sil_glow_cool_temp_rate = 0.01f;
int ps_r2_heatvision = 0;
int heat_vision_cooldown = 1;
float heat_vision_cooldown_time = 20000.0f;
int heat_vision_zombie_cold = 0;

#undef DEDICATED_RENDER_FACTORY_IMPLEMENT

#endif // DEDICATED_SERVER
