#include "stdafx.h"
#include "Level.h"
#include "xrServer.h"
#include "xrServerMapSync.h"

void xrServer::OnProcessClientMapData(NET_Packet& P, ClientID const& clientID)
{
#ifdef DEBUG
	Msg("--- Sending map data to client 0x%08x", clientID);
#endif // #ifdef DEBUG
	NET_Packet responseP;
	string128 client_map_name;
	string128 client_map_version;
	u32 client_geom_crc32;

	P.r_stringZ_s(client_map_name);
	P.r_stringZ_s(client_map_version);
	P.r_u32(client_geom_crc32);

	const shared_str server_map_name = level_name(GetConnectOptions());
	const shared_str server_map_version = level_version(GetConnectOptions());
	const bool mapMatches = !xr_strcmp(server_map_name.c_str(), client_map_name) &&
		!xr_strcmp(server_map_version.c_str(), client_map_version);
	const bool geometryMatches = Level().IsChecksumsEqual(client_geom_crc32);
	if (Core.Params && strstr(Core.Params, "-zone_server_bootstrap_trace"))
		Msg("* [zone-map-sync] server=%s/%s client=%s/%s client_geom=%08x map_match=%d geom_match=%d",
			server_map_name.c_str(), server_map_version.c_str(), client_map_name, client_map_version,
			client_geom_crc32, mapMatches, geometryMatches);

	responseP.w_begin(M_SV_MAP_NAME);

	if (!mapMatches)
	{
		responseP.w_u8(static_cast<u8>(YouHaveOtherMap));
#ifdef DEBUG
		Msg("--- Client [0x%08x] has incorrect map [%s] or version [%s]",
			client_map_name, client_map_version);
#endif // #ifdef DEBUG
		//here we can make hard disconnect of this client...
	}
	else if (!geometryMatches)
	{
		responseP.w_u8(static_cast<u8>(InvalidChecksum));
	}
	else
	{
		responseP.w_u8(static_cast<u8>(SuccessSync));
	}

	SendTo(clientID, responseP, net_flags(TRUE, TRUE));
}
