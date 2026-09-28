#include "stdafx.h"
#include "level.h"
#include "xrServerMapSync.h"
#include "../xrCore/stream_reader.h"
#include "MainMenu.h"
#include "string_table.h"
#include "../xrEngine/xr_ioconsole.h"

static const u32 r_buffer_size = 131072; //128 Kb
static u32 calculate_level_geom_crc32(LPCSTR path_alias, LPCSTR file_name)
{
	void* read_buffer = _alloca(r_buffer_size);
	CStreamReader* geom = FS.rs_open(path_alias, file_name);
	R_ASSERT2(geom, "failed to open level.geom file");
	u32 remaind = geom->elapsed();
	u32 checksum = 0;
	while (remaind)
	{
		u32 to_read = remaind;
		if (remaind > r_buffer_size)
		{
			to_read = r_buffer_size;
		}
		geom->r(read_buffer, to_read);
		checksum ^= crc32(read_buffer, to_read);
		remaind = geom->elapsed();
	}
	FS.r_close(geom);
	return checksum;
}

void CLevel::CalculateLevelCrc32()
{
	Msg("* calculating checksum of level.geom");
	map_data.m_level_geom_crc32 = calculate_level_geom_crc32("$level$", "level.geom");
}

void CLevel::CalculateLevelCrc32ForLevel(LPCSTR level_name)
{
	R_ASSERT(level_name && level_name[0]);
	string_path geom_path;
	xr_sprintf(geom_path, "%s\\level.geom", level_name);
	Msg("* calculating checksum of %s", geom_path);
	map_data.m_level_geom_crc32 = calculate_level_geom_crc32("$game_levels$", geom_path);
}

bool CLevel::IsChecksumsEqual(u32 check_sum) const
{
	return check_sum == map_data.m_level_geom_crc32;
}

bool CLevel::synchronize_map_data()
{
	if (!OnClient() && !IsDemoSave())
	{
		deny_m_spawn = FALSE;
		map_data.m_map_sync_received = true;
		return synchronize_client();
	}

#ifndef MASTER_GOLD
	Msg					("* synchronizing map data...");
#endif // #ifndef MASTER_GOLD

	map_data.CheckToSendMapSync();

#ifdef DEBUG
	Msg("--- Waiting for server map name...");
#endif // #ifdef DEBUG
	ClientReceive();

	if ((map_data.m_wait_map_time >= 1000) && (!map_data.m_map_sync_received) && !IsDemoPlay()) //about 5 seconds
	{
		Msg("Wait map data time out: reconnecting...");
		MakeReconnect();
		g_loading_events.erase(++g_loading_events.begin(), g_loading_events.end());
		return true;
	}

	if (!map_data.m_map_sync_received)
	{
		Sleep(5);
		++map_data.m_wait_map_time;
		return false;
	}

	if (map_data.IsInvalidMapOrVersion())
	{
		Msg("! Incorect map or version, reconnecting...");
		MakeReconnect();
		g_loading_events.erase(++g_loading_events.begin(), g_loading_events.end());
		return true;
	}
	if (map_data.IsInvalidClientChecksum())
	{
		connected_to_server = FALSE;
		return false; //!!!
	}
	return synchronize_client();
}

bool CLevel::synchronize_client()
{
	//---------------------------------------------------------------------------
	if (!sended_request_connection_data)
	{
		NET_Packet P;
		P.w_begin(M_CLIENT_REQUEST_CONNECTION_DATA);

		Send(P, net_flags(TRUE, TRUE, TRUE, TRUE));
		if (Core.Params && strstr(Core.Params, "-zone_server_bootstrap_trace"))
			Msg("* [zone-client-handshake] sent M_CLIENT_REQUEST_CONNECTION_DATA");
		sended_request_connection_data = TRUE;
	}
	//---------------------------------------------------------------------------
	if (game_configured)
	{
		if (Core.Params && strstr(Core.Params, "-zone_server_bootstrap_trace"))
			Msg("* [zone-client-handshake] received server configuration completion");
		deny_m_spawn = FALSE;
		return true;
	}
#ifdef DEBUG
	Msg("--- Waiting for server configuration...");
#endif // #ifdef DEBUG
	if (Server)
	{
		ClientReceive();
		Server->Update();
	} // if OnClient ClientReceive method called in upper invokation
	//Sleep(5); 
	return !!game_configured;
}


void LevelMapSyncData::CheckToSendMapSync()
{
	if (!m_sended_map_name_request)
	{
		if (Core.Params && strstr(Core.Params, "-zone_server_bootstrap_trace"))
			Msg("* [zone-map-sync] client request map=%s version=%s geom=%08x",
				m_name.c_str(), m_map_version.c_str(), m_level_geom_crc32);

		NET_Packet P;
		P.w_begin(M_SV_MAP_NAME);
		P.w_stringZ(m_name);
		P.w_stringZ(m_map_version);
		P.w_u32(m_level_geom_crc32);
		Level().Send(P, net_flags(TRUE, TRUE, TRUE, TRUE));
		m_sended_map_name_request = true;
		invalid_geom_checksum = false;
		m_map_sync_received = false;
		m_wait_map_time = 0;
	}
}

void LevelMapSyncData::ReceiveServerMapSync(NET_Packet& P)
{
	m_map_sync_received = true;
	MapSyncResponse server_resp = static_cast<MapSyncResponse>(P.r_u8());
	if (Core.Params && strstr(Core.Params, "-zone_server_bootstrap_trace"))
		Msg("* [zone-map-sync] server response=%u", static_cast<u32>(server_resp));
	if (server_resp == InvalidChecksum)
	{
		invalid_geom_checksum = true;
	}
	else if (server_resp == YouHaveOtherMap)
	{
		invalid_map_or_version = true;
	}
}
