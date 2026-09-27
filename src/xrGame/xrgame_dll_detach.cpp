#include "pch_script.h"
#include "ai_space.h"
#include "object_factory.h"
#include "ai/monsters/ai_monster_squad_manager.h"
#include "string_table.h"
#ifdef DEDICATED_SERVER
#include "../xrEngine/DedicatedServer.h"
#endif // DEDICATED_SERVER

#include "entity_alive.h"
#include "ui/UIInventoryUtilities.h"
#include "UI/UIXmlInit.h"
#include "UI/UItextureMaster.h"

#include "InfoPortion.h"
#include "PhraseDialog.h"
#include "GameTask.h"
#include "encyclopedia_article.h"

#include "character_info.h"
#include "specific_character.h"
#include "character_community.h"
#include "monster_community.h"
#include "character_rank.h"
#include "character_reputation.h"

#include "profiler.h"

#include "sound_collection_storage.h"
#include "relation_registry.h"

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS;
extern STORY_PAIRS story_ids;
extern STORY_PAIRS spawn_story_ids;

extern void show_smart_cast_stats();
extern void clear_smart_cast_stats();
extern void release_smart_cast_stats();
extern void dump_list_wnd();
extern void dump_list_lines();
extern void dump_list_sublines();
extern void clean_wnd_rects();
extern void dump_list_xmls();
extern void CreateUIGeom();
extern void DestroyUIGeom();
extern void InitHudSoundSettings();

#include "../xrEngine/IGame_Persistent.h"

void init_game_globals()
{
	#ifndef DEDICATED_SERVER
	CreateUIGeom();
	InitHudSoundSettings();
	#endif // DEDICATED_SERVER
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("initializing character metadata");
	#endif // DEDICATED_SERVER
	if (!g_dedicated_server)
	{
		//		CInfoPortion::InitInternal					();
		//.		CEncyclopediaArticle::InitInternal			();
		CPhraseDialog::InitInternal();
		InventoryUtilities::CreateShaders();
	};
	CCharacterInfo::InitInternal();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("character metadata initialized");
	#endif // DEDICATED_SERVER
	CSpecificCharacter::InitInternal();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("specific characters initialized");
	#endif // DEDICATED_SERVER
	CHARACTER_COMMUNITY::InitInternal();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("communities initialized");
	#endif // DEDICATED_SERVER
	CHARACTER_RANK::InitInternal();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("character ranks initialized");
	#endif // DEDICATED_SERVER
	CHARACTER_REPUTATION::InitInternal();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("character reputations initialized");
	#endif // DEDICATED_SERVER
	MONSTER_COMMUNITY::InitInternal();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("monster communities initialized");
	#endif // DEDICATED_SERVER
}

extern CUIXml* g_uiSpotXml;
extern CUIXml* pWpnScopeXml;

extern void destroy_lua_wpn_params();

void clean_game_globals()
{
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("game globals cleanup entered");
	#endif // DEDICATED_SERVER
	destroy_lua_wpn_params();
	// destroy ai space
	xr_delete(g_ai_space);
	// destroy object factory
	xr_delete(g_object_factory);
	// destroy monster squad global var
	xr_delete(g_monster_squad);
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("gameplay registries destroyed");
	#endif // DEDICATED_SERVER

	story_ids.clear();
	spawn_story_ids.clear();

	if (!g_dedicated_server)
	{
		//.		CInfoPortion::DeleteSharedData					();
		//.		CInfoPortion::DeleteIdToIndexData				();

		//.		CEncyclopediaArticle::DeleteSharedData			();
		//.		CEncyclopediaArticle::DeleteIdToIndexData		();

		CPhraseDialog::DeleteSharedData();
		CPhraseDialog::DeleteIdToIndexData();

		InventoryUtilities::DestroyShaders();
	}
	CCharacterInfo::DeleteSharedData();
	CCharacterInfo::DeleteIdToIndexData();

	CSpecificCharacter::DeleteSharedData();
	CSpecificCharacter::DeleteIdToIndexData();

	CHARACTER_COMMUNITY::DeleteIdToIndexData();
	CHARACTER_RANK::DeleteIdToIndexData();
	CHARACTER_REPUTATION::DeleteIdToIndexData();
	MONSTER_COMMUNITY::DeleteIdToIndexData();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("character metadata released");
	#endif // DEDICATED_SERVER


	//static shader for blood
	CEntityAlive::UnloadBloodyWallmarks();
	CEntityAlive::UnloadFireParticles();
	//очищение памяти таблицы строк
	CStringTable::Destroy();
	#ifndef DEDICATED_SERVER
	// Очищение таблицы цветов
	CUIXmlInit::DeleteColorDefs();
	// Очищение таблицы идентификаторов рангов и отношений сталкеров
	InventoryUtilities::ClearCharacterInfoStrings();

	xr_delete(g_sound_collection_storage);
	#endif // DEDICATED_SERVER

#ifdef DEBUG
	xr_delete										(g_profiler);
	release_smart_cast_stats						();
#endif

	RELATION_REGISTRY::clear_relation_registry();
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("simulation registries released");
	#endif // DEDICATED_SERVER

	#ifndef DEDICATED_SERVER
	dump_list_wnd();
	dump_list_lines();
	dump_list_sublines();
	clean_wnd_rects();
	xr_delete(g_uiSpotXml);
	dump_list_xmls();
	DestroyUIGeom();
	xr_delete(pWpnScopeXml);
	CUITextureMaster::FreeTexInfo();
	#endif // DEDICATED_SERVER
	#ifdef DEDICATED_SERVER
	TraceDedicatedServerBootstrap("game globals cleanup completed");
	#endif // DEDICATED_SERVER
}
