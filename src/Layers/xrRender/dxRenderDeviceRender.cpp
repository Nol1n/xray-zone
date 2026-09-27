#include "stdafx.h"
#include "dxRenderDeviceRender.h"

#include "ResourceManager.h"

#if defined(USE_DX11)
#include "gpu_capture.h"
#include "../../xrCore/FS_impl.h"
#include <array>
#include <chrono>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

extern ENGINE_API BOOL g_appLoaded;
extern bool IsMainMenuActive();

namespace
{
// Immediate render context owner only. No worker touches these queries or CSV.
class GpuFrameCapture
{
	using Clock = std::chrono::steady_clock;
	static constexpr size_t MarkerCount = static_cast<size_t>(zone_gpu::Marker::Count);
	static constexpr size_t RegionCount = MarkerCount + 1;
	struct Slot
	{
		ID3D11Query* disjoint = nullptr;
		ID3D11Query* start = nullptr;
		ID3D11Query* end = nullptr;
		std::array<ID3D11Query*, MarkerCount> markers = {};
		std::array<double, RegionCount> cpuRegions = {};
		std::array<double, RegionCount> gpuRegions = {};
		size_t markerCount = 0;
		bool markerOrderValid = true;
		u32 generation = 0, width = 0, height = 0;
		unsigned long long sample = 0;
		u32 frame = 0;
		double elapsedMs = 0, cpuRenderMs = 0, presentMs = 0;
		bool gameplay = false, secondary = false, presented = false, pending = false;
	};
	std::array<Slot, 8> slots;
	FILE* file = nullptr;
	bool initialized = false, failed = false;
	int active = -1;
	unsigned long long sampleCount = 0;
	u32 generation = 0;
	Clock::time_point origin, lastSample, lastFlush, cpuStart;
	Clock::time_point regionStart;

	void write(const Slot& slot, const char* status, double gpuMs = -1)
	{
		if (!file)
			return;
		fprintf(file, "%llu,%.6f,%lu,%s,%d,%d,%s,", slot.sample, slot.elapsedMs,
			static_cast<unsigned long>(slot.frame), slot.secondary ? "pip" : "main",
			slot.gameplay ? 1 : 0, slot.presented ? 1 : 0, status);
		if (gpuMs >= 0)
			fprintf(file, "%.6f", gpuMs);
		fprintf(file, ",%.6f,%.6f,%lu,%lu,%lu,%s", slot.cpuRenderMs, slot.presentMs,
			static_cast<unsigned long>(slot.generation), static_cast<unsigned long>(slot.width),
			static_cast<unsigned long>(slot.height), !passesEnabled() ? "disabled" :
			(gpuMs < 0 ? "unavailable" : (slot.markerOrderValid && slot.markerCount == MarkerCount ? "ok" : "not_recorded")));
		for (size_t index = 0; index < RegionCount; ++index)
		{
			fprintf(file, ",");
			if (passesEnabled() && gpuMs >= 0 && slot.markerOrderValid && slot.markerCount == MarkerCount)
				fprintf(file, "%.6f", slot.gpuRegions[index]);
		}
		for (size_t index = 0; index < RegionCount; ++index)
		{
			fprintf(file, ",");
			if (passesEnabled() && gpuMs >= 0 && slot.markerOrderValid && slot.markerCount == MarkerCount)
				fprintf(file, "%.6f", slot.cpuRegions[index]);
		}
		fprintf(file, "\n");
	}

	bool open(Clock::time_point now)
	{
		string_path path, filename;
		snprintf(filename, sizeof(filename), "engine-gpu-%lu.csv", static_cast<unsigned long>(GetCurrentProcessId()));
		FS.update_path(path, "$logs$", filename);
		int descriptor = -1;
		if (_sopen_s(&descriptor, path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_TEXT,
			_SH_DENYWR, _S_IREAD | _S_IWRITE) == 0)
		{
			file = _fdopen(descriptor, "w");
			if (!file) _close(descriptor);
		}
		if (!file)
		{
			failed = true;
			Msg("! [zone-gpu] could not create CSV (existing files preserved): %s", path);
			return false;
		}
		origin = lastFlush = now;
		fprintf(file, "sample,elapsed_ms,frame,view,gameplay,present_called,status,gpu_ms,cpu_render_ms,present_ms,generation,width,height,pass_status"
			",before_scene_gpu_ms,prepare_gpu_ms,geometry_gpu_ms,lighting_gpu_ms,combine_gpu_ms,after_scene_gpu_ms"
			",before_scene_cpu_ms,prepare_cpu_ms,geometry_cpu_ms,lighting_cpu_ms,combine_cpu_ms,after_scene_cpu_ms\n");
		fflush(file);
		Msg("* [zone-gpu] writing sampled DX11 timings to %s; 100 ms cadence, 8 slots, DONOTFLUSH", path);
		return true;
	}

	bool createQueries()
	{
		D3D11_QUERY_DESC description = {};
		for (Slot& slot : slots)
		{
			description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
			HRESULT result = HW.pDevice->CreateQuery(&description, &slot.disjoint);
			description.Query = D3D11_QUERY_TIMESTAMP;
			if (SUCCEEDED(result)) result = HW.pDevice->CreateQuery(&description, &slot.start);
			if (SUCCEEDED(result)) result = HW.pDevice->CreateQuery(&description, &slot.end);
			if (passesEnabled())
				for (ID3D11Query*& marker : slot.markers)
					if (SUCCEEDED(result)) result = HW.pDevice->CreateQuery(&description, &marker);
			if (FAILED(result))
			{
				failed = true;
				Slot unavailable;
				unavailable.sample = ++sampleCount;
				unavailable.frame = Device.dwFrame;
				unavailable.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - origin).count();
				write(unavailable, "query_create_failed");
				Msg("! [zone-gpu] query creation failed: 0x%08lx", static_cast<unsigned long>(result));
				releaseQueries("cancelled");
				return false;
			}
		}
		++generation;
		Msg("* [zone-gpu] query generation %lu ready: %lux%lu; passes=%d", static_cast<unsigned long>(generation),
			static_cast<unsigned long>(Device.dwWidth), static_cast<unsigned long>(Device.dwHeight), passesEnabled() ? 1 : 0);
		initialized = true;
		return true;
	}

	void poll()
	{
		for (Slot& slot : slots)
		{
			if (!slot.pending)
				continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT reliability = {};
			UINT64 start = 0, end = 0;
			std::array<UINT64, MarkerCount> boundaries = {};
			HRESULT result = HW.pContext->GetData(slot.disjoint, &reliability, sizeof(reliability), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (result == S_OK) result = HW.pContext->GetData(slot.start, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (result == S_OK) result = HW.pContext->GetData(slot.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (result == S_OK && passesEnabled() && slot.markerOrderValid && slot.markerCount == MarkerCount)
				for (size_t index = 0; index < MarkerCount && result == S_OK; ++index)
					result = HW.pContext->GetData(slot.markers[index], &boundaries[index], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (result == S_FALSE)
				continue; // Try on a later frame; no wait, flush or query reuse.
			if (FAILED(result)) write(slot, "query_error");
			else if (reliability.Disjoint || reliability.Frequency == 0 || end < start) write(slot, "disjoint");
			else
			{
				bool ordered = true;
				UINT64 previous = start;
				if (passesEnabled() && slot.markerOrderValid && slot.markerCount == MarkerCount)
				{
					for (size_t index = 0; index < RegionCount; ++index)
					{
						const UINT64 current = index < MarkerCount ? boundaries[index] : end;
						if (current < previous || current > end) { ordered = false; break; }
						slot.gpuRegions[index] = static_cast<double>(current - previous) * 1000.0 / static_cast<double>(reliability.Frequency);
						previous = current;
					}
				}
				if (!ordered) write(slot, "query_error");
				else write(slot, "ok", static_cast<double>(end - start) * 1000.0 / static_cast<double>(reliability.Frequency));
			}
			slot.pending = false;
		}
	}

public:
	static bool passesEnabled()
	{
		static const bool enabled = [] {
			if (!Core.Params) return false;
			const char* flag = "-zone_gpu_passes";
			const size_t length = strlen(flag);
			for (const char* found = strstr(Core.Params, flag); found; found = strstr(found + length, flag))
				if ((found == Core.Params || found[-1] == ' ' || found[-1] == '\t') &&
					(found[length] == 0 || found[length] == ' ' || found[length] == '\t')) return true;
			return false;
		}();
		return enabled;
	}
	static bool enabled()
	{
		static const bool enabled = [] {
			if (!Core.Params) return false;
			const char* flag = "-zone_gpu_capture";
			const size_t length = strlen(flag);
			for (const char* found = strstr(Core.Params, flag); found; found = strstr(found + length, flag))
				if ((found == Core.Params || found[-1] == ' ' || found[-1] == '\t') &&
					(found[length] == 0 || found[length] == ' ' || found[length] == '\t')) return true;
			return false;
		}();
		return enabled;
	}

	~GpuFrameCapture() { if (file) fclose(file); }

	void beginFrame()
	{
		if (!enabled() || failed)
			return;
		const Clock::time_point now = Clock::now();
		if (!file && !open(now)) return;
		if (!initialized && !createQueries()) return;
		poll();
		if (now - lastFlush >= std::chrono::seconds(1))
		{
			lastFlush = now;
			if (fflush(file) != 0 || ferror(file))
			{
				failed = true;
				Msg("! [zone-gpu] CSV write failed");
				return;
			}
		}
		if (now - lastSample < std::chrono::milliseconds(100)) return;
		lastSample = now;
		Slot attempt;
		attempt.sample = ++sampleCount;
		attempt.frame = Device.dwFrame;
		attempt.generation = generation; attempt.width = Device.dwWidth; attempt.height = Device.dwHeight;
		attempt.elapsedMs = std::chrono::duration<double, std::milli>(now - origin).count();
		attempt.secondary = Device.m_SecondViewport.IsSVPFrame();
		attempt.gameplay = g_pGameLevel && g_pGameLevel->bReady && g_appLoaded && !Device.dwPrecacheFrame &&
			Device.b_is_Active && !Device.Paused() && !IsMainMenuActive();
		for (size_t index = 0; index < slots.size(); ++index)
		{
			if (slots[index].pending) continue;
			Slot& slot = slots[index];
			slot.sample = attempt.sample; slot.frame = attempt.frame; slot.elapsedMs = attempt.elapsedMs;
			slot.gameplay = attempt.gameplay; slot.secondary = attempt.secondary;
			slot.generation = attempt.generation; slot.width = attempt.width; slot.height = attempt.height;
			slot.markerCount = 0; slot.markerOrderValid = true;
			slot.cpuRenderMs = slot.presentMs = 0; slot.presented = false;
			active = static_cast<int>(index);
			cpuStart = Clock::now();
			regionStart = cpuStart;
			HW.pContext->Begin(slot.disjoint);
			HW.pContext->End(slot.start);
			return;
		}
		write(attempt, "ring_full"); // Bounded pressure is visible, not a zero GPU time.
	}

	bool recording() const { return active >= 0; }
	void mark(zone_gpu::Marker marker)
	{
		if (!recording() || !passesEnabled()) return;
		Slot& slot = slots[active];
		const size_t index = static_cast<size_t>(marker);
		if (index != slot.markerCount || index >= MarkerCount)
		{
			slot.markerOrderValid = false;
			return;
		}
		HW.pContext->End(slot.markers[index]);
		const Clock::time_point now = Clock::now();
		slot.cpuRegions[index] = std::chrono::duration<double, std::milli>(now - regionStart).count();
		regionStart = now;
		++slot.markerCount;
	}
	void endRender()
	{
		if (!recording()) return;
		Slot& slot = slots[active];
		HW.pContext->End(slot.end);
		HW.pContext->End(slot.disjoint);
		const Clock::time_point now = Clock::now();
		slot.cpuRenderMs = std::chrono::duration<double, std::milli>(now - cpuStart).count();
		if (passesEnabled() && slot.markerCount == MarkerCount)
			slot.cpuRegions[MarkerCount] = std::chrono::duration<double, std::milli>(now - regionStart).count();
	}
	void finishFrame(bool presented, double presentMs)
	{
		if (!recording()) return;
		Slot& slot = slots[active];
		slot.presented = presented; slot.presentMs = presentMs; slot.pending = true;
		active = -1;
	}
	void releaseQueries(const char* reason)
	{
		for (size_t index = 0; index < slots.size(); ++index)
		{
			Slot& slot = slots[index];
			if (slot.pending || active == static_cast<int>(index)) write(slot, reason);
			slot.pending = false;
			_RELEASE(slot.disjoint); _RELEASE(slot.start); _RELEASE(slot.end);
			for (ID3D11Query*& marker : slot.markers) _RELEASE(marker);
		}
		active = -1; initialized = false;
		if (file) fflush(file);
	}
} gpuFrameCapture;
}

void zone_gpu::mark(Marker marker) { gpuFrameCapture.mark(marker); }
#endif

dxRenderDeviceRender::dxRenderDeviceRender()
	: Resources(0)
{
	;
}

void dxRenderDeviceRender::Copy(IRenderDeviceRender& _in)
{
	*this = *(dxRenderDeviceRender*)&_in;
}

void dxRenderDeviceRender::setGamma(float fGamma)
{
	m_Gamma.Gamma(fGamma);
}

void dxRenderDeviceRender::setBrightness(float fGamma)
{
	m_Gamma.Brightness(fGamma);
}

void dxRenderDeviceRender::setContrast(float fGamma)
{
	m_Gamma.Contrast(fGamma);
}

void dxRenderDeviceRender::updateGamma()
{
	m_Gamma.Update();
}

void dxRenderDeviceRender::OnDeviceDestroy(BOOL bKeepTextures)
{
#if defined(USE_DX11)
	gpuFrameCapture.releaseQueries("cancelled_destroy");
#endif
	m_WireShader.destroy();
	m_SelectionShader.destroy();

	Resources->OnDeviceDestroy(bKeepTextures);
	RCache.OnDeviceDestroy();
}

void dxRenderDeviceRender::ValidateHW()
{
	HW.Validate();
}

void dxRenderDeviceRender::DestroyHW()
{
	xr_delete(Resources);
	HW.DestroyDevice();
}

void dxRenderDeviceRender::Reset(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2)
{
#if defined(USE_DX11)
	gpuFrameCapture.releaseQueries("cancelled_reset");
#endif
#ifdef DEBUG
    _SHOW_REF("*ref -CRenderDevice::ResetTotal: DeviceREF:",HW.pDevice);
#endif // DEBUG

	Resources->reset_begin();
	Memory.mem_compact();

#if !defined(USE_DX10) && !defined(USE_DX11)
	const bool noTexturesInRAM = RImplementation.o.no_ram_textures;
	if (noTexturesInRAM)
		ResourcesDeferredUnload();
#endif

	HW.Reset(hWnd);

#if defined(USE_DX11)
	dwWidth = HW.m_ChainDesc.Width;
	dwHeight = HW.m_ChainDesc.Height;
#elif defined(USE_DX10)
	dwWidth = HW.m_ChainDesc.BufferDesc.Width;
	dwHeight = HW.m_ChainDesc.BufferDesc.Height;
#else	//	USE_DX10
	if (noTexturesInRAM)
		ResourcesDeferredUpload();

	dwWidth = HW.DevPP.BackBufferWidth;
	dwHeight = HW.DevPP.BackBufferHeight;
#endif	//	USE_DX10

	fWidth_2 = float(dwWidth / 2);
	fHeight_2 = float(dwHeight / 2);
	Resources->reset_end();

#ifdef DEBUG
    _SHOW_REF("*ref +CRenderDevice::ResetTotal: DeviceREF:",HW.pDevice);
#endif // DEBUG
}

void dxRenderDeviceRender::SetupStates()
{
	HW.Caps.Update();

#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement Resetting of render states into default mode
	//VERIFY(!"dxRenderDeviceRender::SetupStates not implemented.");
	SSManager.SetMaxAnisotropy(ps_r__tf_Anisotropic);
	SSManager.SetMipLODBias(ps_r__tf_Mipbias);
#else	//	USE_DX10
	for (u32 i = 0; i < HW.Caps.raster.dwStages; i++)
	{
		CHK_DX(HW.pDevice->SetSamplerState(i, D3DSAMP_MAXANISOTROPY, ps_r__tf_Anisotropic));
		CHK_DX(HW.pDevice->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, *(LPDWORD)&ps_r__tf_Mipbias));
		CHK_DX(HW.pDevice->SetSamplerState ( i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR ));
		CHK_DX(HW.pDevice->SetSamplerState ( i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR ));
		CHK_DX(HW.pDevice->SetSamplerState ( i, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR ));
	}
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_DITHERENABLE, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_COLORVERTEX, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_ZENABLE, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_SHADEMODE, D3DSHADE_GOURAUD ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_CULLMODE, D3DCULL_CCW ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_ALPHAFUNC, D3DCMP_GREATER ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_LOCALVIEWER, TRUE ));

	CHK_DX(HW.pDevice->SetRenderState( D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_SPECULARMATERIALSOURCE,D3DMCS_MATERIAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_EMISSIVEMATERIALSOURCE,D3DMCS_COLOR1 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_MULTISAMPLEANTIALIAS, FALSE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_NORMALIZENORMALS, TRUE ));

	if (psDeviceFlags.test(rsWireframe))
		RCache.set_FillMode(D3DFILL_WIREFRAME);
	else
		RCache.set_FillMode(D3DFILL_SOLID);

	// ******************** Fog parameters
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGCOLOR, 0 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_RANGEFOGENABLE, FALSE ));
	if (HW.Caps.bTableFog)
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGTABLEMODE, D3DFOG_LINEAR ));
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGVERTEXMODE, D3DFOG_NONE ));
	}
	else
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGTABLEMODE, D3DFOG_NONE ));
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR ));
	}

#endif	//	USE_DX10
}

void dxRenderDeviceRender::OnDeviceCreate(LPCSTR shName)
{
	// Signal everyone - device created
	RCache.OnDeviceCreate();
	m_Gamma.Update();
	Resources->OnDeviceCreate(shName);
	::Render->create();
	Device.Statistic->OnDeviceCreate();

	//#ifndef DEDICATED_SERVER
	if (!g_dedicated_server)
	{
		m_WireShader.create("editor\\wire");
		m_SelectionShader.create("editor\\selection");

		DUImpl.OnDeviceCreate();
	}
	//#endif
}

void dxRenderDeviceRender::Create(HWND hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2,
                                  bool move_window)
{
	HW.CreateDevice(hWnd, move_window);
#if defined(USE_DX11)
	dwWidth = HW.m_ChainDesc.Width;
	dwHeight = HW.m_ChainDesc.Height;
#elif defined(USE_DX10)
	dwWidth = HW.m_ChainDesc.BufferDesc.Width;
	dwHeight = HW.m_ChainDesc.BufferDesc.Height;
#else	//	USE_DX10
	dwWidth = HW.DevPP.BackBufferWidth;
	dwHeight = HW.DevPP.BackBufferHeight;
#endif	//	USE_DX10
	fWidth_2 = float(dwWidth / 2);
	fHeight_2 = float(dwHeight / 2);
	Resources = xr_new<CResourceManager>();
}

void dxRenderDeviceRender::SetupGPU(BOOL bForceGPU_SW, BOOL bForceGPU_NonPure, BOOL bForceGPU_REF)
{
	HW.Caps.bForceGPU_SW = bForceGPU_SW;
	HW.Caps.bForceGPU_NonPure = bForceGPU_NonPure;
	HW.Caps.bForceGPU_REF = bForceGPU_REF;
}

void dxRenderDeviceRender::overdrawBegin()
{
#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement overdrawBegin
	VERIFY(!"dxRenderDeviceRender::overdrawBegin not implemented.");
#else	//	USE_DX10
	// Turn stenciling
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILENABLE, TRUE ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_ALWAYS ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILREF, 0 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILMASK, 0x00000000 ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILWRITEMASK, 0xffffffff ));

	// Increment the stencil buffer for each pixel drawn
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILPASS, D3DSTENCILOP_INCRSAT ));

	if (1 == HW.Caps.SceneMode)
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP ));
	} // Overdraw
	else
	{
		CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILZFAIL, D3DSTENCILOP_INCRSAT ));
	} // ZB access
#endif	//	USE_DX10
}

void dxRenderDeviceRender::overdrawEnd()
{
#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement overdrawEnd
	VERIFY(!"dxRenderDeviceRender::overdrawBegin not implemented.");
#else	//	USE_DX10
	// Set up the stencil states
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILPASS, D3DSTENCILOP_KEEP ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_EQUAL ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILMASK, 0xff ));

	// Set the background to black
	CHK_DX(HW.pDevice->Clear(0,0,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),0,0));

	// Draw a rectangle wherever the count equal I
	RCache.OnFrameEnd();
	CHK_DX(HW.pDevice->SetFVF( FVF::F_TL ));

	// Render gradients
	for (int I = 0; I < 12; I++)
	{
		u32 _c = I * 256 / 13;
		u32 c = D3DCOLOR_XRGB(_c, _c, _c);

		FVF::TL pv[4];
		pv[0].set(float(0), float(Device.dwHeight), c, 0, 0);
		pv[1].set(float(0), float(0), c, 0, 0);
		pv[2].set(float(Device.dwWidth), float(Device.dwHeight), c, 0, 0);
		pv[3].set(float(Device.dwWidth), float(0), c, 0, 0);

		CHK_DX(HW.pDevice->SetRenderState ( D3DRS_STENCILREF, I ));
		CHK_DX(HW.pDevice->DrawPrimitiveUP ( D3DPT_TRIANGLESTRIP, 2, pv, sizeof(FVF::TL) ));
	}
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_STENCILENABLE, FALSE ));
#endif	//	USE_DX10
}

void dxRenderDeviceRender::DeferredLoad(BOOL E)
{
	Resources->DeferredLoad(E);
}

void dxRenderDeviceRender::ResourcesDeferredUpload()
{
	Resources->DeferredUpload();
}

void dxRenderDeviceRender::ResourcesDeferredUnload()
{
	Resources->DeferredUnload();
}

void dxRenderDeviceRender::ResourcesPrefetchCreateTexture(LPCSTR name)
{
	Resources->_CreateTexture(name);
}

void dxRenderDeviceRender::ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps)
{
	if (Resources)
		Resources->_GetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);
}

void dxRenderDeviceRender::ResourcesStoreNecessaryTextures()
{
	dxRenderDeviceRender::Instance().Resources->StoreNecessaryTextures();
}

void dxRenderDeviceRender::ResourcesDumpMemoryUsage()
{
	dxRenderDeviceRender::Instance().Resources->_DumpMemoryUsage();
}

dxRenderDeviceRender::DeviceState dxRenderDeviceRender::GetDeviceState()
{
	HW.Validate();
#if defined(USE_DX10) || defined(USE_DX11)
    HRESULT _hr = HW.m_pSwapChain->Present(0, DXGI_PRESENT_TEST);

	if (FAILED(_hr))
	{
		//LV: Check if it's correct. If so - that should be fix for AMD issues. Thank me later. Yo
		// If the device was lost, do not render until we get it back
		if (DXGI_ERROR_DEVICE_REMOVED == _hr)
			return dsLost;

		// Check if the device is ready to be reset
		if (DXGI_ERROR_DEVICE_RESET == _hr)
			return dsNeedReset;
	}
#else	//	USE_DX10
	HRESULT _hr = HW.pDevice->TestCooperativeLevel();

	if (FAILED(_hr))
	{
		// If the device was lost, do not render until we get it back
		if (D3DERR_DEVICELOST == _hr)
			return dsLost;

		// Check if the device is ready to be reset
		if (D3DERR_DEVICENOTRESET == _hr)
			return dsNeedReset;
	}
#endif	//	USE_DX10

	return dsOK;
}

BOOL dxRenderDeviceRender::GetForceGPU_REF()
{
	return HW.Caps.bForceGPU_REF;
}

u32 dxRenderDeviceRender::GetCacheStatPolys()
{
	return RCache.stat.polys;
}

void dxRenderDeviceRender::Begin()
{
#if defined(USE_DX11)
	gpuFrameCapture.beginFrame();
#endif
#if !defined(USE_DX10) && !defined(USE_DX11)
	CHK_DX(HW.pDevice->BeginScene());
#endif	//	USE_DX10
	RCache.OnFrameBegin();
	RCache.set_CullMode(CULL_CW);
	RCache.set_CullMode(CULL_CCW);
	if (HW.Caps.SceneMode) overdrawBegin();
}

void dxRenderDeviceRender::Clear()
{
#if defined(USE_DX10) || defined(USE_DX11)
	HW.pContext->ClearDepthStencilView(RCache.get_ZB(),
	                                   D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);

	if (psDeviceFlags.test(rsClearBB))
	{
		FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
	}
#else	//	USE_DX10
	CHK_DX(HW.pDevice->Clear(0,0,
		D3DCLEAR_ZBUFFER|
		(psDeviceFlags.test(rsClearBB)?D3DCLEAR_TARGET:0)|
		(HW.Caps.bStencil?D3DCLEAR_STENCIL:0),
		D3DCOLOR_XRGB(0,0,0),1,0
	));
#endif	//	USE_DX10
}

void DoAsyncScreenshot();

void dxRenderDeviceRender::End()
{
	VERIFY(HW.pDevice);

	if (HW.Caps.SceneMode) overdrawEnd();

#if defined(USE_DX11)
	gpuFrameCapture.endRender();
#endif
	RCache.OnFrameEnd();
	Memory.dbg_check();

	DoAsyncScreenshot();

#if defined(USE_DX10) || defined(USE_DX11)
    UINT present_flags = 0;
	bool use_vsync = !!psDeviceFlags.test(rsVSync);
	UINT present_interval = (use_vsync) ? 1 : 0;

# if defined(USE_DX11)
	// NOTE: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
    BOOL is_windowed = HW.m_ChainDescFullscreen.Windowed;
	if (is_windowed && !use_vsync && HW.m_SupportsVRR) {
        present_flags |= DXGI_PRESENT_ALLOW_TEARING;
	}
# endif

	if (!Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady) {
# if defined(USE_DX11)
		const auto presentStart = gpuFrameCapture.recording() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
# endif
		HW.m_pSwapChain->Present(present_interval, present_flags);
# if defined(USE_DX11)
		if (gpuFrameCapture.recording())
			gpuFrameCapture.finishFrame(true, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count());
# endif
	}
# if defined(USE_DX11)
	else gpuFrameCapture.finishFrame(false, 0);
# endif
#else //!USE_DX10 || USE_DX11
	CHK_DX(HW.pDevice->EndScene());

	if (!Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady)
		HW.pDevice->Present(NULL, NULL, NULL, NULL);
#endif //-USE_DX10
	//HRESULT _hr		= HW.pDevice->Present( NULL, NULL, NULL, NULL );
	//if				(D3DERR_DEVICELOST==_hr)	return;			// we will handle this later
}

void dxRenderDeviceRender::ResourcesDestroyNecessaryTextures()
{
	Resources->DestroyNecessaryTextures();
}

void dxRenderDeviceRender::ClearTarget()
{
#if defined(USE_DX10) || defined(USE_DX11)
	FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	HW.pContext->ClearRenderTargetView(RCache.get_RT(), ColorRGBA);
#else	//	USE_DX10
	CHK_DX(HW.pDevice->Clear(0,0,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,0),1,0));
#endif	//	USE_DX10
}

void dxRenderDeviceRender::SetCacheXform(Fmatrix& mView, Fmatrix& mProject)
{
	RCache.set_xform_view(mView);
	RCache.set_xform_project(mProject);
}

void dxRenderDeviceRender::SetCacheXform_prev(Fmatrix& mView, Fmatrix& mProject)
{
	RCache.set_xform_view_prev(mView);
	RCache.set_xform_project_prev(mProject);
}

bool dxRenderDeviceRender::HWSupportsShaderYUV2RGB()
{
	u32 v_dev = CAP_VERSION(HW.Caps.raster_major, HW.Caps.raster_minor);
	u32 v_need = CAP_VERSION(2, 0);
	return (v_dev >= v_need);
}

void dxRenderDeviceRender::OnAssetsChanged()
{
	Resources->m_textures_description.UnLoad();
	Resources->m_textures_description.Load();
}

extern ENGINE_API void SetStartupMonitor(HMONITOR h);
extern XRAPI_API xr_token* vid_mode_token;
#if defined(USE_DX11) || defined(USE_DX10)
void fill_vid_mode_list(CHW* _hw);
void free_vid_mode_list();
#endif

// Windowed: keep user's pick if present in vid_mode_token, else monitor native.
// Borderless/fullscreen: always monitor native.  Moves the window to match.
// Caller must have set HW.m_pOutput / vid_mode_token to reflect the target
// monitor before calling.
static void FinalizeMonitorGeometry(const MONITORINFO& mi, HWND hWnd,
                                    u32 g_screenmode_,
                                    u32& vidModeW, u32& vidModeH)
{
    const int monX = mi.rcMonitor.left;
    const int monY = mi.rcMonitor.top;
    const int monW = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    u32 finalW = (u32)monW;
    u32 finalH = (u32)monH;
    if (g_screenmode_ == 0)
    {
        string32 cur_buf;
        xr_sprintf(cur_buf, sizeof(cur_buf), "%ux%u", vidModeW, vidModeH);
        for (xr_token* t = vid_mode_token; t && t->name; ++t)
        {
            if (!xr_strcmp(t->name, cur_buf))
            {
                finalW = vidModeW;
                finalH = vidModeH;
                break;
            }
        }
    }
    vidModeW = finalW;
    vidModeH = finalH;

    int wx, wy, ww = (int)finalW, wh = (int)finalH;
    if (g_screenmode_ == 0)
    {
        wx = monX + (monW - ww) / 2;
        wy = monY + (monH - wh) / 2;
    }
    else
    {
        wx = monX;
        wy = monY;
    }
    SetWindowPos(hWnd, HWND_TOP, wx, wy, ww, wh,
                 SWP_FRAMECHANGED | SWP_NOCOPYBITS | SWP_DRAWFRAME);
}

bool dxRenderDeviceRender::SwitchOutputMonitor(HMONITOR hTargetMon, HWND hWnd,
                                               u32 g_screenmode_,
                                               u32& vidModeW, u32& vidModeH)
{
    if (hTargetMon == NULL)
        return false;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hTargetMon, &mi))
    {
        Msg("! vid_monitor: GetMonitorInfoA failed for target monitor");
        return false;
    }

#if defined(USE_DX11) || defined(USE_DX10)
    IDXGIOutput* new_output = HW.FindOutputOnCurrentAdapter(hTargetMon);
    if (!new_output)
    {
        Msg("! vid_monitor: target monitor is not on the current adapter, restart to apply");
        return false;
    }

    SetStartupMonitor(hTargetMon);

    if (g_screenmode_ == 2)
        HW.m_pSwapChain->SetFullscreenState(FALSE, NULL); // best-effort

    _RELEASE(HW.m_pOutput);
    HW.m_pOutput = new_output;

    free_vid_mode_list();
    fill_vid_mode_list(&HW);

    FinalizeMonitorGeometry(mi, hWnd, g_screenmode_, vidModeW, vidModeH);
    Msg("* vid_monitor: output swapped, final mode %ux%u", vidModeW, vidModeH);
    return true;

#elif !defined(USE_DX10) && !defined(USE_DX11)
    UINT target_adapter = UINT(-1);
    for (UINT a = 0; a < HW.pD3D->GetAdapterCount(); ++a)
    {
        if (HW.pD3D->GetAdapterMonitor(a) == hTargetMon)
        {
            target_adapter = a;
            break;
        }
    }
    if (target_adapter == UINT(-1))
    {
        Msg("! vid_monitor: target monitor not found among D3D9 adapters, restart to apply");
        return false;
    }

    if (target_adapter != HW.DevAdapter)
    {
        D3DADAPTER_IDENTIFIER9 id_cur, id_new;
        HRESULT hr1 = HW.pD3D->GetAdapterIdentifier(HW.DevAdapter, 0, &id_cur);
        HRESULT hr2 = HW.pD3D->GetAdapterIdentifier(target_adapter,  0, &id_new);
        const bool same_gpu = SUCCEEDED(hr1) && SUCCEEDED(hr2)
                           && memcmp(&id_cur.DeviceIdentifier,
                                     &id_new.DeviceIdentifier,
                                     sizeof(GUID)) == 0;
        if (!same_gpu)
        {
            Msg("! vid_monitor: target monitor is on a different D3D9 adapter, restart to apply");
            return false;
        }
    }
    if (g_screenmode_ == 2)
    {
        Msg("! vid_monitor: DX9 exclusive fullscreen across monitors requires restart to apply");
        return false;
    }

    SetStartupMonitor(hTargetMon);

    FinalizeMonitorGeometry(mi, hWnd, g_screenmode_, vidModeW, vidModeH);
    Msg("* vid_monitor: DX9 output swapped, final mode %ux%u", vidModeW, vidModeH);
    return true;

#else
    return false;
#endif
}
