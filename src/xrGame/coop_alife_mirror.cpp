#include "stdafx.h"
#include "pch_script.h"
#include "coop_alife_mirror.h"
#include "Level.h"
#include "GameObject.h"
#include "ai_space.h"
#include "script_engine.h"
#include "game_graph.h"
#include "level_graph.h"
#include "ai_object_location.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "InventoryOwner.h"

static CCoopAlifeMirror* g_coop_alife_mirror = NULL;
LPCSTR alife_section_name = "alife";

CCoopAlifeMirror::~CCoopAlifeMirror()
{
	for (xr_map<u16, CSE_Abstract*>::iterator I = m_entities.begin(); I != m_entities.end(); ++I)
		F_entity_Destroy(I->second);
	m_entities.clear();
	m_story.clear();
}

CCoopAlifeMirror* CCoopAlifeMirror::instance()
{
	if (!IsGameTypeCoop() || !g_pGameLevel || OnServer()) return NULL;
	if (!g_coop_alife_mirror) g_coop_alife_mirror = xr_new<CCoopAlifeMirror>();
	return g_coop_alife_mirror;
}

void CCoopAlifeMirror::clear_instance()
{
	xr_delete(g_coop_alife_mirror);
}

bool CCoopAlifeMirror::keep_spawned(CSE_Abstract* entity)
{
	CCoopAlifeMirror* mirror = instance();
	if (!mirror || !entity) return false;
	mirror->keep(entity);
	return true;
}

void CCoopAlifeMirror::on_destroy(u16 id)
{
	if (g_coop_alife_mirror) g_coop_alife_mirror->remove(id);
}

void CCoopAlifeMirror::keep(CSE_Abstract* entity)
{
	remove(entity->ID);
	m_entities[entity->ID] = entity;
	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(entity);
	if (alife_object && alife_object->m_story_id != INVALID_STORY_ID) m_story[alife_object->m_story_id] = entity->ID;
}

void CCoopAlifeMirror::remove(u16 id)
{
	xr_map<u16, CSE_Abstract*>::iterator I = m_entities.find(id);
	if (I == m_entities.end()) return;
	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(I->second);
	if (alife_object && alife_object->m_story_id != INVALID_STORY_ID)
	{
		xr_map<ALife::_STORY_ID, u16>::iterator S = m_story.find(alife_object->m_story_id);
		if (S != m_story.end() && S->second == id) m_story.erase(S);
	}
	F_entity_Destroy(I->second);
	m_entities.erase(I);
}

void CCoopAlifeMirror::placeholder(u16 id, LPCSTR section, u16 parent, const Fvector& position)
{
	if (!section || !*section || !pSettings->section_exist(section)) return;
	CSE_Abstract* entity = F_entity_Create(section);
	if (!entity) return;
	entity->ID = id;
	entity->ID_Parent = parent;
	entity->o_Position = position;
	string256 name;
	xr_sprintf(name, "%s%u", section, u32(id));
	entity->set_name_replace(name);
	CSE_ALifeDynamicObject* dynamic = smart_cast<CSE_ALifeDynamicObject*>(entity);
	if (dynamic) dynamic->m_bOnline = false;
	keep(entity);
}

CSE_Abstract* CCoopAlifeMirror::entity(u16 id) const
{
	xr_map<u16, CSE_Abstract*>::const_iterator I = m_entities.find(id);
	return I == m_entities.end() ? NULL : I->second;
}

// The entity is the spawn-time snapshot; what moves is the client object.
void CCoopAlifeMirror::refresh(CSE_Abstract* entity) const
{
	CObject* object = Level().Objects.net_Find(entity->ID);
	if (!object || object->getDestroy()) return;
	entity->o_Position = object->Position();
	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(entity);
	CGameObject* game_object = smart_cast<CGameObject*>(object);
	if (alife_object && game_object)
	{
		if (ai().level_graph().valid_vertex_id(game_object->ai_location().level_vertex_id()))
			alife_object->m_tNodeID = game_object->ai_location().level_vertex_id();
		if (ai().get_game_graph() && ai().game_graph().valid_vertex_id(game_object->ai_location().game_vertex_id()))
			alife_object->m_tGraphID = game_object->ai_location().game_vertex_id();
	}
	CSE_ALifeDynamicObject* dynamic = smart_cast<CSE_ALifeDynamicObject*>(entity);
	if (dynamic) dynamic->m_bOnline = true;
	// the parent changes hands here (items taken and dropped): the client object knows it
	entity->ID_Parent = game_object && game_object->H_Parent() ? game_object->H_Parent()->ID() : entity->ID_Parent;
}

CSE_ALifeDynamicObject* CCoopAlifeMirror::object(ALife::_OBJECT_ID id)
{
	CSE_Abstract* e = entity(id);
	if (!e) return NULL;
	refresh(e);
	return smart_cast<CSE_ALifeDynamicObject*>(e);
}

CSE_ALifeDynamicObject* CCoopAlifeMirror::story_object(ALife::_STORY_ID id)
{
	xr_map<ALife::_STORY_ID, u16>::const_iterator I = m_story.find(id);
	return I == m_story.end() ? NULL : object(I->second);
}

CSE_ALifeCreatureActor* CCoopAlifeMirror::actor()
{
	CObject* body = Level().CurrentControlEntity();
	if (!body) return NULL;
	return smart_cast<CSE_ALifeCreatureActor*>(object(body->ID()));
}

u32 CCoopAlifeMirror::level_id() const
{
	return ai().get_level_graph() ? u32(ai().level_graph().level_id()) : u32(-1);
}

LPCSTR CCoopAlifeMirror::level_name(int level_id) const
{
	if (!ai().get_game_graph()) return NULL;
	const GameGraph::LEVEL_MAP& levels = ai().game_graph().header().levels();
	GameGraph::LEVEL_MAP::const_iterator I = levels.find((GameGraph::_LEVEL_ID)level_id);
	if (I == levels.end()) return NULL;
	return *ai().game_graph().header().level((GameGraph::_LEVEL_ID)level_id).name();
}

float CCoopAlifeMirror::switch_distance() const
{
	return pSettings->r_float(alife_section_name, "switch_distance");
}

// The world's info portions are mirrored into the Lua book (coop_client_actor.world_infos):
// has_alife_info there answers for the actor; other objects only through their client object.
bool CCoopAlifeMirror::has_info(const ALife::_OBJECT_ID& id, LPCSTR info_id)
{
	CObject* body = Level().CurrentControlEntity();
	if (id == 0 || (body && id == body->ID()))
	{
		::luabind::functor<bool> has_alife_info;
		if (ai().script_engine().functor("has_alife_info", has_alife_info)) return has_alife_info(info_id);
		return false;
	}
	CInventoryOwner* owner = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(id));
	return owner ? owner->HasInfo(info_id) : false;
}

// coop_client_actor.alife_create_id asks the server and waits for the id (game_cl_Coop::CreateWait);
// the mirror then holds the object, a stand-in until its spawn arrives.
static CSE_Abstract* lua_create(CCoopAlifeMirror* mirror, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, int ammo = -1)
{
	::luabind::functor<int> create;
	if (!ai().script_engine().functor("coop_client_actor.alife_create_id", create)) return NULL;
	const int id = create(section, position, level_vertex_id, game_vertex_id, int(id_parent == ALife::_OBJECT_ID(-1) ? -1 : id_parent), ammo);
	if (id < 0) return NULL;
	return mirror->entity(u16(id));
}

CSE_ALifeDynamicObject* CCoopAlifeMirror::create_by_spawn_id(ALife::_SPAWN_ID spawn_id)
{
	unsupported("create(spawn_id)");
	return NULL;
}

CSE_Abstract* CCoopAlifeMirror::create(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id)
{
	return lua_create(this, section, position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1));
}

CSE_Abstract* CCoopAlifeMirror::create2(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent)
{
	return lua_create(this, section, position, level_vertex_id, game_vertex_id, id_parent);
}

CSE_Abstract* CCoopAlifeMirror::create3(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, bool)
{
	return lua_create(this, section, position, level_vertex_id, game_vertex_id, id_parent);
}

CSE_Abstract* CCoopAlifeMirror::create_ammo(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, int ammo_to_spawn)
{
	if (ammo_to_spawn <= 0) return NULL;
	return lua_create(this, section, position, level_vertex_id, game_vertex_id, id_parent, ammo_to_spawn);
}

void CCoopAlifeMirror::release(CSE_Abstract* object, bool)
{
	if (!object) return;
	::luabind::functor<void> release_id;
	if (ai().script_engine().functor("alife_release_id", release_id)) release_id(u32(object->ID));
}

// The modded exes' alife():register(se) registers an entity made with alife():create(..., false) and
// returns it (respawned). Here every creation is the server's at once (coop_client_actor's
// create_and_wait): the stand-in comes back as it is. Without it the GAMMA flows that create,
// adjust and then register (a weapon part taken off: arti_jamming_repairs.remove_part) stopped at
// the call - the part was made, the weapon kept it.
CSE_Abstract* CCoopAlifeMirror::register_object(CSE_Abstract* object)
{
	return object;
}

void CCoopAlifeMirror::iterate_objects(const luabind::functor<bool>& functor)
{
	xr_vector<u16> ids;
	ids.reserve(m_entities.size());
	for (xr_map<u16, CSE_Abstract*>::const_iterator I = m_entities.begin(); I != m_entities.end(); ++I)
		ids.push_back(I->first); // the functor may create or release: iterate a copy of the keys
	for (u32 i = 0; i < ids.size(); ++i)
	{
		CSE_ALifeDynamicObject* dynamic = object(ids[i]);
		if (dynamic && functor(dynamic)) break;
	}
}

void CCoopAlifeMirror::unsupported(LPCSTR what)
{
	shared_str key(what);
	if (m_reported.find(key) != m_reported.end()) return;
	m_reported.insert(key);
	Msg("! [COOP_ALIFE] client mirror: %s is the server's (ignored here)", what);
}

// Lua: level.coop_alife() on a client; coop_client_actor installs it as alife()
CCoopAlifeMirror* g_coop_alife()
{
	return CCoopAlifeMirror::instance();
}

#pragma optimize("s", on)
void CCoopAlifeMirror::script_register(lua_State* L)
{
	using namespace luabind;
	module(L)
	[
		class_<CCoopAlifeMirror>("coop_alife_mirror")
		.def("valid_object_id", &CCoopAlifeMirror::valid_object_id)
		.def("level_id", &CCoopAlifeMirror::level_id)
		.def("level_name", &CCoopAlifeMirror::level_name)
		.def("object", &CCoopAlifeMirror::object)
		.def("object", &CCoopAlifeMirror::object2)
		.def("story_object", &CCoopAlifeMirror::story_object)
		.def("actor", &CCoopAlifeMirror::actor)
		.def("has_info", &CCoopAlifeMirror::has_info)
		.def("dont_has_info", &CCoopAlifeMirror::dont_has_info)
		.def("create", &CCoopAlifeMirror::create_by_spawn_id)
		.def("create", &CCoopAlifeMirror::create)
		.def("create", &CCoopAlifeMirror::create2)
		.def("create", &CCoopAlifeMirror::create3)
		.def("create_ammo", &CCoopAlifeMirror::create_ammo)
		.def("release", &CCoopAlifeMirror::release)
		.def("register", &CCoopAlifeMirror::register_object)
		.def("set_switch_online", &CCoopAlifeMirror::set_switch_online)
		.def("set_switch_offline", &CCoopAlifeMirror::set_switch_offline)
		.def("set_interactive", &CCoopAlifeMirror::set_interactive)
		.def("kill_entity", &CCoopAlifeMirror::kill_entity)
		.def("teleport_object", &CCoopAlifeMirror::teleport_object)
		.def("set_objects_per_update", &CCoopAlifeMirror::set_objects_per_update)
		.def("switch_distance", &CCoopAlifeMirror::switch_distance)
		.def("set_switch_distance", &CCoopAlifeMirror::set_switch_distance)
		.def("object_count", &CCoopAlifeMirror::object_count)
		.def("iterate_objects", &CCoopAlifeMirror::iterate_objects)
		.def("uses_player_anchors", &CCoopAlifeMirror::uses_player_anchors)
	];
	module(L, "level")
	[
		def("coop_alife", &g_coop_alife)
	];
}
