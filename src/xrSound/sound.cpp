#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreA.h"

class CNoSoundManager final : public CSound_manager_interface
{
protected:
	void _initialize(int) override {}
	void _clear() override {}
	void _create_data(ref_sound_data& data, LPCSTR, esound_type sound_type, int game_type) override
	{
		data.handle = nullptr;
		data.feedback = nullptr;
		data.s_type = sound_type;
		data.g_type = game_type;
		data.g_object = nullptr;
		data.dwBytesTotal = 0;
		data.fTimeTotal = 0.f;
	}
	void _destroy_data(ref_sound_data&) override {}

public:
	void _restart() override {}
	BOOL i_locked() override { return FALSE; }
	BOOL is_ready() override { return FALSE; }
	void refresh_devices() override {}
	void default_device_changed() override {}
	void switch_device(LPCSTR) override {}
	void create(ref_sound&, LPCSTR, esound_type, int) override {}
	void attach_tail(ref_sound&, LPCSTR) override {}
	void clone(ref_sound&, const ref_sound&, esound_type, int) override {}
	void destroy(ref_sound& sound) override { sound._p = nullptr; }
	void stop_emitters() override {}
	int pause_emitters(bool) override { return 0; }
	void play(ref_sound&, CObject*, u32, float) override {}
	void play_at_pos(ref_sound&, CObject*, const Fvector&, u32, float) override {}
	void play_no_feedback(ref_sound&, CObject*, u32, float, Fvector*, float*, float*, Fvector2*) override {}
	void set_master_volume(float) override {}
	void set_geometry_env(IReader*) override {}
	void set_geometry_som(IReader*) override {}
	void set_geometry_occ(CDB::MODEL*) override {}
	void set_handler(sound_event*) override {}
	void update(const Fvector&, const Fvector&, const Fvector&) override {}
	void statistic(CSound_stats*, CSound_stats_ext*) override {}
	float get_occlusion_to(const Fvector&, const Fvector&, float) override { return 1.f; }
	float get_occlusion(Fvector&, float, Fvector*) override { return 1.f; }
	void object_relcase(CObject*) override {}
	const Fvector& listener_position() override
	{
		static const Fvector position{};
		return position;
	}
};

XRSOUND_API xr_token* snd_devices_token = NULL;
XRSOUND_API xr_string snd_device_name;

void CSound_manager_interface::_create(int stage)
{
	if (stage == 0)
	{
		SoundRenderA = xr_new<CSoundRender_CoreA>();
		SoundRender = SoundRenderA;
		Sound = SoundRender;

		if (strstr(Core.Params, "-nosound"))
		{
			SoundRender->bPresent = FALSE;
			return;
		}
		else
			SoundRender->bPresent = TRUE;
	}

	if (!SoundRender->bPresent) return;
	Sound->_initialize(stage);
}

void CSound_manager_interface::_create_silent()
{
	if (!Sound)
		Sound = xr_new<CNoSoundManager>();
}

void CSound_manager_interface::_destroy()
{
	if (!Sound)
		return;

	Sound->_clear();
	if (SoundRender)
		xr_delete(SoundRender);
	else
		xr_delete(Sound);
	Sound = 0;
}
