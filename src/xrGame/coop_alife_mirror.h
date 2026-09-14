#pragma once
#include "alife_space.h"
#include "game_graph_space.h"
#include "script_export_space.h"

class CSE_Abstract;
class CSE_ALifeDynamicObject;
class CSE_ALifeCreatureActor;

// Coop client: the server entities the client has seen, kept as the read-only ALife the SP scripts
// expect from alife(). Every M_SPAWN carries the full STATE of the object; the client used to parse
// it, spawn the client object and delete the entity. The mirror keeps it (script classes included:
// se_stalker, se_item... are created by the same object factory), drops it on GE_DESTROY, refreshes
// the position of an online object from its client object on every read, and answers the
// alife_simulator Lua interface: reads from the registry, writes as the item verbs of
// coop_client_actor (alife_create / alife_release) or a logged no-op. Offline objects are not here
// (they were never sent): object() answers nil for them, as it did for everything before.
class CCoopAlifeMirror
{
	xr_map<u16, CSE_Abstract*> m_entities;
	xr_map<ALife::_STORY_ID, u16> m_story;
	xr_set<shared_str> m_reported; // unsupported calls, logged once each
	void refresh(CSE_Abstract* entity) const;
public:
	~CCoopAlifeMirror();
	static CCoopAlifeMirror* instance(); // NULL unless a coop client
	static void clear_instance();
	static bool keep_spawned(CSE_Abstract* entity); // true: the mirror owns it now
	static void on_destroy(u16 id);
	void keep(CSE_Abstract* entity);
	void remove(u16 id);
	// The server's answer to a creation came before the object's spawn: a bare entity of the section
	// with the id, parent and position stands in (alife():object(id) is valid at once, as in SP); the
	// real M_SPAWN replaces it through keep().
	void placeholder(u16 id, LPCSTR section, u16 parent, const Fvector& position);
	u32 count() const { return u32(m_entities.size()); }
	CSE_Abstract* entity(u16 id) const;

	// alife_simulator interface (Lua)
	CSE_ALifeDynamicObject* object(ALife::_OBJECT_ID id);
	CSE_ALifeDynamicObject* object2(ALife::_OBJECT_ID id, bool no_assert) { return object(id); }
	CSE_ALifeDynamicObject* story_object(ALife::_STORY_ID id);
	CSE_ALifeCreatureActor* actor();
	bool valid_object_id(ALife::_OBJECT_ID id) const { return id != ALife::_OBJECT_ID(-1); }
	u32 level_id() const;
	LPCSTR level_name(int level_id) const;
	float switch_distance() const;
	bool has_info(const ALife::_OBJECT_ID& id, LPCSTR info_id);
	bool dont_has_info(const ALife::_OBJECT_ID& id, LPCSTR info_id) { return !has_info(id, info_id); }
	CSE_ALifeDynamicObject* create_by_spawn_id(ALife::_SPAWN_ID spawn_id);
	CSE_Abstract* create(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id);
	CSE_Abstract* create2(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent);
	CSE_Abstract* create3(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, bool reg);
	CSE_Abstract* create_ammo(LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, int ammo_to_spawn);
	void release(CSE_Abstract* object, bool);
	void set_switch_online(ALife::_OBJECT_ID id, bool value) { unsupported("set_switch_online"); }
	void set_switch_offline(ALife::_OBJECT_ID id, bool value) { unsupported("set_switch_offline"); }
	void set_interactive(ALife::_OBJECT_ID id, bool value) { unsupported("set_interactive"); }
	void kill_entity(ALife::_OBJECT_ID id) { unsupported("kill_entity"); }
	void teleport_object(ALife::_OBJECT_ID id, GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, const Fvector& position) { unsupported("teleport_object"); }
	void set_objects_per_update(u32 count) {}
	void set_switch_distance(float) {}
	u32 object_count() const { return count(); }
	bool uses_player_anchors() const { return false; }
	void unsupported(LPCSTR what);
	static void script_register(lua_State* L);
};
