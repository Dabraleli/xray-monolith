////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_binder.h"
#include "xrServer_Objects_ALife.h"
#include "script_binder_object.h"
#include "script_game_object.h"
#include "gameobject.h"
#include "level.h"
#include "game_sv_coop.h"

// comment next string when commiting
//#define DBG_DISABLE_SCRIPTS

CScriptBinder::CScriptBinder()
{
	init();
}

CScriptBinder::~CScriptBinder()
{
	VERIFY(!m_object);
}

void CScriptBinder::init()
{
	m_object = 0;
}

void CScriptBinder::clear()
{
	try
	{
		xr_delete(m_object);
	}
	catch (...)
	{
		m_object = 0;
	}
	init();
}

void CScriptBinder::reinit()
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
	if (m_object)
	{
		try
		{
			m_object->reinit();
		}
		catch (...)
		{
			clear();
		}
	}
#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::reinit() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER
}

void CScriptBinder::Load(LPCSTR section)
{
}

void CScriptBinder::reload(LPCSTR section)
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
#ifndef DBG_DISABLE_SCRIPTS
	// Reject before invoking Lua: deleting a binder cannot undo its constructor.
    const bool coop_replica = IsGameTypeCoop() && !OnServer();
    if (coop_replica && (!smart_cast<CGameObject*>(this)->Local() ||
        !pSettings->line_exist(section, "script_binding") ||
        xr_strcmp(pSettings->r_string(section, "script_binding"), "bind_stalker.actor_init")))
        return;
	VERIFY(!m_object);
	if (!pSettings->line_exist(section, "script_binding"))
		return;

    LPCSTR binding = pSettings->r_string(section, "script_binding");
    if (coop_replica) binding = "coop_client_actor.actor_init";
    if (IsGameTypeCoop() && OnServer() && strstr(Core.Params, "-coop_server_probe") &&
        !xr_strcmp(binding, "bind_stalker.actor_init"))
        binding = "coop_server_actor.actor_init";
	::luabind::functor<void> lua_function;
	if (!ai().script_engine().functor(binding, lua_function))
	{
		ai().script_engine().script_log(ScriptStorage::eLuaMessageTypeError, "function %s is not loaded!",
		                                pSettings->r_string(section, "script_binding"));
		return;
	}

	CGameObject* game_object = smart_cast<CGameObject*>(this);

	try
	{
		lua_function(game_object ? game_object->lua_game_object() : 0);
	}
	catch (...)
	{
		clear();
		return;
	}

	if (m_object)
	{
		try
		{
			m_object->reload(section);
		}
		catch (...)
		{
			clear();
		}
	}
#endif
#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::reload() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER
}

BOOL CScriptBinder::net_Spawn(CSE_Abstract* DC)
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
	CSE_Abstract* abstract = (CSE_Abstract*)DC;
	CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(abstract);
	if (object && m_object)
	{
		try
		{
			return ((BOOL)m_object->net_Spawn(object));
		}
		catch (...)
		{
			clear();
		}
	}

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::net_Spawn() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER

	return (TRUE);
}

void CScriptBinder::net_Destroy()
{
	if (m_object)
	{
#ifdef _DEBUG
		Msg						("* Core object %s is UNbinded from the script object",smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // _DEBUG
		try
		{
			m_object->net_Destroy();
		}
		catch (...)
		{
			clear();
		}
	}
	xr_delete(m_object);
}

void CScriptBinder::set_object(CScriptBinderObject* object)
{
	if (IsGameTypeSingle() || (IsGameTypeCoop() && (OnServer() || smart_cast<CGameObject*>(this)->Local())))
	{
		VERIFY2(!m_object, "Cannot bind to the object twice!");
#ifdef _DEBUG
		Msg					("* Core object %s is binded with the script object",smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // _DEBUG
		m_object = object;
	}
	else
	{
		xr_delete(object);
	}
}

void CScriptBinder::shedule_Update(u32 time_delta)
{
	if (m_object)
	{
		// Coop server: every binder (doors, restrictors, zones, campfires, items) runs its Lua with
		// the nearest player body as the actor — or its Lua owner's body (companions follow the
		// player who recruited them). NPC updates already sit in such a scope (nested).
		// Actor binders (world actor, bodies) are coop-owned and address players explicitly.
		CGameObject* owner = smart_cast<CGameObject*>(this);
		CoopLuaActor coop_actor(owner && !owner->cast_actor() ? game_sv_Coop::ContextBodyFor(owner) : NULL, false);
		try
		{
			m_object->shedule_Update(time_delta);
		}
		catch (...)
		{
			// A C++ exception (an access violation under /EHa included) ends the binder for good:
			// say so, or the object silently stops running its Lua (a dedicated coop server lost
			// its world actor to a HUD getter this way). The coop server's actor binders (the world
			// actor: every world service; the bodies) stay: the tick that threw is lost, the next
			// runs - a dropped world actor is a dead server (18.09: a mod's packet parser on a body).
			const bool keep = IsGameTypeCoop() && OnServer() && owner && owner->cast_actor();
			Msg("! script binder update of [%s][%u] raised an exception: the binder is %s", owner ? owner->cName().c_str() : "?", owner ? owner->ID() : 0, keep ? "kept (coop actor)" : "dropped");
			if (!keep) clear();
		}
	}
}

void CScriptBinder::save(NET_Packet& output_packet)
{
	if (m_object)
	{
		try
		{
			m_object->save(&output_packet);
		}
		catch (...)
		{
			clear();
		}
	}
}

void CScriptBinder::load(IReader& input_packet)
{
	if (m_object)
	{
		try
		{
			m_object->load(&input_packet);
		}
		catch (...)
		{
			clear();
		}
	}
}

BOOL CScriptBinder::net_SaveRelevant()
{
	if (m_object)
	{
		try
		{
			return (m_object->net_SaveRelevant());
		}
		catch (...)
		{
			clear();
		}
	}
	return (FALSE);
}

void CScriptBinder::net_Relcase(CObject* object)
{
	CGameObject* game_object = smart_cast<CGameObject*>(object);
	if (m_object && game_object)
	{
		try
		{
			m_object->net_Relcase(game_object->lua_game_object());
		}
		catch (...)
		{
			clear();
		}
	}
}
