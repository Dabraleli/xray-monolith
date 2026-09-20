////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator.cpp
//	Created 	: 25.12.2002
//  Modified 	: 13.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "alife_simulator.h"
#include "xrServer_Objects_ALife.h"
#include "ai_space.h"
#include "../xrEngine/IGame_Persistent.h"
#include "script_engine.h"
#include "mainmenu.h"
#include "object_factory.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "level.h"
#include "game_graph.h"
#include "../xrEngine/xr_ioconsole.h"
#include "game_sv_coop.h"

#ifdef DEBUG
#	include "moving_objects.h"
#endif // DEBUG

LPCSTR alife_section = "alife";

bool CALifeSimulator::uses_player_anchors() const
{
    return IsGameTypeCoop();
}

float CALifeSimulator::activation_distance(const Fvector& position, u32 game_vertex_id) const
{
    if (!uses_player_anchors())
        return graph().actor()->o_Position.distance_to(position);

    // The connected bodies are the spatial anchors; the world actor is identity only, except below.
    float nearest = flt_max;
    if (!ai().game_graph().valid_vertex_id(game_vertex_id)) return nearest;
    const auto level_id = ai().game_graph().vertex(game_vertex_id)->level_id();
    auto on_level = [&](const CSE_ALifeCreatureActor* body)
    {
        return ai().game_graph().valid_vertex_id(body->m_tGraphID) &&
            ai().game_graph().vertex(body->m_tGraphID)->level_id() == level_id;
    };
    IClient* internal = server().GetServerClient();
    bool connected = false;
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* client = static_cast<xrClientData*>(connection);
        if (!client->ps || !client->owner) return;
        CSE_Abstract* record = server().ID_to_entity(client->ps->GameID);
        if (record != client->owner) return;
        CSE_ALifeCreatureActor* body = smart_cast<CSE_ALifeCreatureActor*>(record);
        if (!body || body == graph().actor() || body->owner != internal) return;
        connected = true;
        if (!on_level(body)) return;
        nearest = _min(nearest, body->o_Position.distance_to(position));
    };
    server().ForEachClientDo(visit);
    if (connected) m_coop_world_anchors = false;
    if (connected || !m_coop_world_anchors) return nearest;
    // No connected body yet: the clients still load after a new game, a load or a level change.
    // Until the first one anchors the world, the bodies waiting for their players (parked: the saved
    // ones, put by the level change where the players appear) anchor it, and the world actor when
    // none is on the level - as the SP actor anchors its level from the first switch pass. Without
    // that nothing was online while the clients loaded, and the squads' scheduled Lua
    // (sim_squad_scripted:check_online_status, the spawn exclusion excl_dist = 75 m) measured against
    // the world actor standing on the arrival point: an offline squad within 75 m of it was kept
    // offline by its can_switch_online for as long as a player stayed near (19.09: the Marsh quest
    // NPC 60 m from the Cordon entrance - the marker on the map, no NPC on the spot).
    xr_vector<CSE_ALifeCreatureActor*> waiting;
    game_sv_Coop::WaitingBodies(waiting);
    bool any = false;
    for (u32 i = 0; i < waiting.size(); ++i)
    {
        if (!on_level(waiting[i])) continue;
        any = true;
        nearest = _min(nearest, waiting[i]->o_Position.distance_to(position));
    }
    if (!any && on_level(graph().actor())) nearest = graph().actor()->o_Position.distance_to(position);
    return nearest;
}

void CALifeSimulator::coop_switch_all_next()
{
    if (!uses_player_anchors()) return;
    graph().level().iterate_as_first_time();
}

extern void destroy_lua_wpn_params();

void restart_all()
{
	if (strstr(Core.Params, "-keep_lua"))
		return;

	destroy_lua_wpn_params();
	MainMenu()->DestroyInternal(true);
	xr_delete(g_object_factory);
	ai().script_engine().init();

#ifdef DEBUG
	ai().moving_objects().clear	();
#endif // DEBUG
}

CALifeSimulator::CALifeSimulator(xrServer* server, shared_str* command_line) :
	CALifeUpdateManager(server, alife_section),
	CALifeInteractionManager(server, alife_section),
	CALifeSimulatorBase(server, alife_section)
{
	m_coop_world_anchors = true;
	restart_all();

	ai().set_alife(this);

	setup_command_line(command_line);

	typedef IGame_Persistent::params params;
	params& p = g_pGamePersistent->m_game_params;

	R_ASSERT2(
		xr_strlen(p.m_game_or_spawn) &&
		!xr_strcmp(p.m_alife,"alife") &&
		(!xr_strcmp(p.m_game_type,"single") || !xr_strcmp(p.m_game_type,"coop")),
		"Invalid server options!"
	);

	string256 temp;
	xr_strcpy(temp, p.m_game_or_spawn);
	xr_strcat(temp, "/");
	xr_strcat(temp, p.m_game_type);
	xr_strcat(temp, "/");
	xr_strcat(temp, p.m_alife);
	*command_line = temp;

	LPCSTR start_game_callback = pSettings->r_string(alife_section, "start_game_callback");
	::luabind::functor<void> functor;
	R_ASSERT2(ai().script_engine().functor(start_game_callback,functor), "failed to get start game callback");
	functor();

	load(p.m_game_or_spawn, !xr_strcmp(p.m_new_or_load, "load") ? false : true, !xr_strcmp(p.m_new_or_load, "new"));
}

CALifeSimulator::~CALifeSimulator()
{
	VERIFY(!ai().get_alife());

	configs_type::iterator i = m_configs_lru.begin();
	configs_type::iterator const e = m_configs_lru.end();
	for (; i != e; ++i)
		FS.r_close((*i).second);
}

void CALifeSimulator::destroy()
{
	//	validate					();
	CALifeUpdateManager::destroy();
	VERIFY(ai().get_alife());
	ai().set_alife(0);
}

void CALifeSimulator::setup_simulator(CSE_ALifeObject* object)
{
	//	VERIFY2						(!object->m_alife_simulator,object->s_name_replace);
	object->m_alife_simulator = this;
}

void CALifeSimulator::reload(LPCSTR section)
{
	CALifeUpdateManager::reload(section);
}

struct string_prdicate
{
	shared_str m_value;

	inline string_prdicate(shared_str const& value) :
		m_value(value)
	{
	}

	inline bool operator( )(std::pair<shared_str, IReader*> const& value) const
	{
		return !xr_strcmp(m_value, value.first);
	}
}; // struct string_prdicate

IReader const* CALifeSimulator::get_config(shared_str config) const
{
	configs_type::iterator const found = std::find_if(m_configs_lru.begin(), m_configs_lru.end(),
	                                                  string_prdicate(config));
	if (found != m_configs_lru.end())
	{
		configs_type::value_type temp = *found;
		m_configs_lru.erase(found);
		m_configs_lru.insert(m_configs_lru.begin(), std::make_pair(temp.first, temp.second));
		return temp.second;
	}

	string_path file_name;
	FS.update_path(file_name, "$game_config$", config.c_str());
	if (!FS.exist(file_name))
		return 0;

	m_configs_lru.insert(m_configs_lru.begin(), std::make_pair(config, FS.r_open(file_name)));
	return m_configs_lru.front().second;
}

namespace detail
{
	bool object_exists_in_alife_registry(u32 id)
	{
		if (ai().get_alife())
		{
			return ai().alife().objects().object((ALife::_OBJECT_ID)id, true) != 0;
		}
		return false;
	}
} // detail
