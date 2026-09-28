#include "stdafx.h"
#include "xrserver.h"
#include "game_sv_single.h"
#include "game_sv_deathmatch.h"
#include "game_sv_teamdeathmatch.h"
#include "game_sv_artefacthunt.h"
#include "xrMessages.h"
#include "game_cl_artefacthunt.h"
#include "game_cl_single.h"
#include "MainMenu.h"
#include "../xrEngine/x_ray.h"
#include "file_transfer.h"
#include "screenshot_server.h"
#include "../xrNetServer/NET_AuthCheck.h"
#ifdef DEDICATED_SERVER
#include "../xrEngine/DedicatedServer.h"
#endif // DEDICATED_SERVER
#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

LPCSTR xrServer::get_map_download_url(LPCSTR level_name, LPCSTR level_version)
{
	R_ASSERT(level_name && level_version);
	LPCSTR ret_url = "";
	CInifile* level_ini = pApp->GetArchiveHeader(level_name, level_version);
	if (!level_ini)
	{
		if (!IsGameTypeSingle())
			Msg("! Warning: level [%s][%s] has not header ltx", level_name, level_version);

		return ret_url;
	}

	ret_url = level_ini->r_string_wb("header", "link").c_str();
	if (!ret_url)
		ret_url = "";

	return ret_url;
}

xrServer::EConnect xrServer::Connect(shared_str& session_name, GameDescriptionData& game_descr)
{
#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect entered");
#endif // DEDICATED_SERVER
#ifdef DEBUG
	Msg						("* sv_Connect: %s",	*session_name);
#endif

	// Parse options and create game
	if (0 == strchr(*session_name, '/'))
		return ErrConnect;
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect parsed session options");
	#endif // DEDICATED_SERVER

	string1024 options;
	R_ASSERT2(xr_strlen(session_name) <= sizeof(options), "session_name too BIIIGGG!!!");
	xr_strcpy(options, strchr(*session_name, '/') + 1);

	// Parse game type
	string1024 type;
	R_ASSERT2(xr_strlen(options) <= sizeof(type), "session_name too BIIIGGG!!!");
	xr_strcpy(type, options);
	if (strchr(type, '/')) *strchr(type, '/') = 0;
	game = NULL;

	CLASS_ID clsid = game_GameState::getCLASS_ID(type, true);
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect resolved game class");
	#endif // DEDICATED_SERVER
	game = smart_cast<game_sv_GameState*>(NEW_INSTANCE(clsid));
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect game state instance created");
	#endif // DEDICATED_SERVER
	if (0 == game) return ErrConnect;

	xr_auth_strings_t tmp_ignore;
	xr_auth_strings_t tmp_check;
	fill_auth_check_params(tmp_ignore, tmp_check);
	FS.auth_generate(tmp_ignore, tmp_check);
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect authentication checksum generated");
	#endif // DEDICATED_SERVER

	// Options
	//	game->type				= type_id;
	if (game->Type() != eGameIDSingle)
	{
		m_file_transfers = xr_new<file_transfer::server_site>();
		initialize_screenshot_proxies();
		LoadServerInfo();
	}
#ifdef DEBUG
	Msg("* Created server_game %s",game->type_name());
#endif

	ZeroMemory(&game_descr, sizeof(game_descr));
	xr_strcpy(game_descr.map_name, game->level_name(session_name.c_str()).c_str());
	xr_strcpy(game_descr.map_version, game_sv_GameState::parse_level_version(session_name.c_str()).c_str());
	xr_strcpy(game_descr.download_url, get_map_download_url(game_descr.map_name, game_descr.map_version));
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect game description built");
	#endif // DEDICATED_SERVER

	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect creating game instance");
	#endif // DEDICATED_SERVER
	game->Create(session_name);
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect game instance created");
		xr_strcpy(game_descr.map_name, game->level_name(session_name.c_str()).c_str());
		xr_strcpy(game_descr.download_url, get_map_download_url(game_descr.map_name, game_descr.map_version));
		Msg("* [zone-server] advertised level: %s", game_descr.map_name);
	#endif // DEDICATED_SERVER

	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect starting network server");
	#endif // DEDICATED_SERVER
	EConnect result = IPureServer::Connect(*session_name, game_descr);
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("xrServer::Connect network server started");
	#endif // DEDICATED_SERVER
	return result;
}


IClient* xrServer::new_client(SClientConnectData* cl_data)
{
	IClient* CL = client_Find_Get(cl_data->clientID);
	VERIFY(CL);

	// copy entity
	CL->ID = cl_data->clientID;
	CL->process_id = cl_data->process_id;
	CL->name = cl_data->name; //only for offline mode
	CL->pass._set(cl_data->pass);

	NET_Packet P;
	P.B.count = 0;
	P.r_pos = 0;

	game->AddDelayedEvent(P, GAME_EVENT_CREATE_CLIENT, 0, CL->ID);

	return CL;
}

void xrServer::AttachNewClient(IClient* CL)
{
	MSYS_CONFIG msgConfig;
	msgConfig.sign1 = 0x12071980;
	msgConfig.sign2 = 0x26111975;

	if (psNET_direct_connect) //single_game
	{
		SV_Client = CL;
		CL->flags.bLocal = 1;
		SendTo_LL(SV_Client->ID, &msgConfig, sizeof(msgConfig), net_flags(TRUE,TRUE,TRUE,TRUE));
	}
	else
	{
		SendTo_LL(CL->ID, &msgConfig, sizeof(msgConfig), net_flags(TRUE, TRUE, TRUE, TRUE));
		Server_Client_Check(CL);
	}

	// gen message
	if (!NeedToCheckClient_GameSpy_CDKey(CL))
	{
		//-------------------------------------------------------------
		Check_GameSpy_CDKey_Success(CL);
	}

	//xrClientData * CL_D=(xrClientData*)(CL); 
	//ip_address				ClAddress;
	//GetClientAddress		(CL->ID, ClAddress);
	CL->m_guid[0] = 0;
}

void xrServer::RequestClientDigest(IClient* CL)
{
	if (IsGameTypeSingle() || (CL == GetServerClient()))
	{
		Check_BuildVersion_Success(CL);
		return;
	}
	xrClientData* tmp_client = smart_cast<xrClientData*>(CL);
	VERIFY(tmp_client);
	PerformSecretKeysSync(tmp_client);

	NET_Packet P;
	P.w_begin(M_SV_DIGEST);
	SendTo(CL->ID, P);
}

#define NET_BANNED_STR	"Player banned by server!"

void xrServer::ProcessClientDigest(xrClientData* xrCL, NET_Packet* P)
{
	R_ASSERT(xrCL);
	IClient* tmp_client = static_cast<IClient*>(xrCL);
	game_sv_mp* server_game = smart_cast<game_sv_mp*>(game);
	P->r_stringZ(xrCL->m_cdkey_digest);
	shared_str admin_name;
	if (server_game->IsPlayerBanned(xrCL->m_cdkey_digest.c_str(), admin_name))
	{
		R_ASSERT2(tmp_client != GetServerClient(), "can't disconnect server client");
		Msg("--- Client [%s] tried to connect - rejecting connection (he is banned by %s) ...",
		    tmp_client->m_cAddress.to_string().c_str(),
		    admin_name.size() ? admin_name.c_str() : "Server");
		LPSTR message_to_user;
		if (admin_name.size())
		{
			STRCONCAT(message_to_user, "mp_you_have_been_banned_by ", admin_name.c_str());
		}
		else
		{
			message_to_user = "";
		}
		SendConnectResult(tmp_client, 0, ecr_have_been_banned, message_to_user);
		return;
	}
	GetPooledState(xrCL);
	PerformSecretKeysSync(xrCL);
	Check_BuildVersion_Success(tmp_client);
}
