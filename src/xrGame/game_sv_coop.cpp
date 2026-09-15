#include "stdafx.h"
#include "game_sv_coop.h"
#include "coop_spatial_scope.h"
#include "xrServer.h"
#include "xrMessages.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "Actor.h"
#include "level_graph.h"
#include "game_level_cross_table.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../xrEngine/x_ray.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/Text_Console.h"
#include "PhraseDialog.h"
#include "PhraseDialogManager.h"
#include "InventoryOwner.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_game_object.h"
#include "relation_registry.h"
#include "InventoryBox.h"
#include "GameTask.h"
#include "GametaskManager.h"
#include "map_manager.h"
#include "map_location.h"
#include "game_graph.h"
#include "trade.h"
#include "inventory.h"
#include "inventory_item.h"
#include "eatable_item.h"
#include "Weapon.h"
#include "actorcondition.h"
#include "character_community.h"
#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "visual_memory_manager.h"

extern ENGINE_API bool g_dedicated_server;

// A body starts with a basic kit. Optional [loadout] in coop_server.ltx (section = count)
// replaces the built-in list. Unknown sections are reported and skipped, never fatal.
typedef xr_vector<std::pair<shared_str, u32>> COOP_LOADOUT;

static void coop_read_loadout(COOP_LOADOUT& items, u32& money)
{
    money = 2000; // [loadout] money = N overrides
    string_path config_path;
    FS.update_path(config_path, "$app_data_root$", "coop_server.ltx");
    if (FS.exist(config_path))
    {
        // The DLTX ini cache would otherwise return the copy read at server start (run_seconds);
        // the file is re-read for every body so the kit can be changed while the server runs.
        CInifile::InvalidateCache(config_path);
        CInifile config(config_path);
        if (config.section_exist("loadout"))
        {
            LPCSTR name, value;
            for (u32 i = 0; config.r_line("loadout", i, &name, &value); ++i)
            {
                if (!xr_strcmp(name, "money"))
                {
                    money = value && xr_strlen(value) ? u32(atoi(value)) : money;
                    continue;
                }
                items.push_back(std::make_pair(shared_str(name), value && xr_strlen(value) ? u32(atoi(value)) : 1u));
            }
        }
    }
    if (items.empty())
    {
        static const std::pair<LPCSTR, u32> defaults[] = {
            {"wpn_knife", 1}, {"wpn_pm", 1}, {"ammo_9x18_fmj", 2}, {"wpn_ak74u", 1}, {"ammo_5.45x39_fmj", 2},
            {"novice_outfit", 1}, {"helm_resp", 1}, {"bandage", 2}, {"medkit", 1}, {"device_flashlight", 1},
            {"bolt", 1}, {"bread", 1}};
        for (const auto& entry : defaults) items.push_back(std::make_pair(shared_str(entry.first), entry.second));
    }
}

// The character from the client's join menu (coop_menu.script -> "coop=" client option ->
// SClientConnectData::coop -> IClient::coop_profile): "faction:<f>;icon:<i>;money:<n>;loadout:<sec[*count],...>".
// True when a loadout was given (then it replaces the server's [loadout]).
static bool coop_parse_profile(LPCSTR profile, shared_str& faction, shared_str& icon, u32& money, COOP_LOADOUT& items)
{
    bool has_loadout = false;
    if (!profile || !xr_strlen(profile)) return false;
    xr_string text(profile);
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(';', start);
        if (end == xr_string::npos) end = text.size();
        xr_string field = text.substr(start, end - start);
        start = end + 1;
        const size_t colon = field.find(':');
        if (colon == xr_string::npos) continue;
        const xr_string key = field.substr(0, colon), value = field.substr(colon + 1);
        if (key == "faction" && !value.empty()) faction = value.c_str();
        else if (key == "icon" && !value.empty()) icon = value.c_str();
        else if (key == "money") money = u32(atoi(value.c_str()));
        else if (key == "loadout")
        {
            has_loadout = true;
            size_t item_start = 0;
            while (item_start < value.size())
            {
                size_t item_end = value.find(',', item_start);
                if (item_end == xr_string::npos) item_end = value.size();
                xr_string item = value.substr(item_start, item_end - item_start);
                item_start = item_end + 1;
                u32 count = 1;
                const size_t star = item.find('*');
                if (star != xr_string::npos)
                {
                    count = u32(atoi(item.substr(star + 1).c_str()));
                    item = item.substr(0, star);
                }
                if (!item.empty() && count) items.push_back(std::make_pair(shared_str(item.c_str()), _min(count, 20u)));
            }
        }
    }
    return has_loadout;
}

// The items go to the body once it is online (see below); the money must be on the server entity
// before that: CActor::net_Spawn takes it from the spawn packet (the live object does not exist
// yet when this runs, the internal client creates it from that packet later).
static void coop_spawn_loadout(CALifeSimulator& alife, CSE_ALifeCreatureActor* body, const COOP_LOADOUT& items)
{
    u32 spawned = 0;
    for (const auto& entry : items)
    {
        if (!pSettings->section_exist(entry.first))
        {
            Msg("! [COOP_SERVER] LOADOUT_UNKNOWN body=%u section=%s", body->ID, entry.first.c_str());
            continue;
        }
        for (u32 n = 0; n < entry.second; ++n)
        {
            // The body is already online: a registered ALife child would stay offline and the
            // inventory would never see it. Route through Process_spawn exactly like
            // alife():create(section, position, lvid, gvid, parent) does for online parents.
            NET_Packet packet;
            packet.w_begin(M_SPAWN);
            packet.w_stringZ(entry.first.c_str());
            CSE_Abstract* item = alife.spawn_item(entry.first.c_str(), body->o_Position, body->m_tNodeID, body->m_tGraphID, body->ID, false);
            item->Spawn_Write(packet, FALSE);
            alife.server().FreeID(item->ID, 0);
            F_entity_Destroy(item);
            ClientID clientID;
            clientID.set(0xffff);
            u16 dummy;
            packet.r_begin(dummy);
            if (alife.server().Process_spawn(packet, clientID)) ++spawned;
        }
    }
    Msg("[COOP_SERVER] LOADOUT body=%u entries=%u spawned=%u", body->ID, u32(items.size()), spawned);
}

static void coop_store_load(xr_map<shared_str, xr_string>& store, LPCSTR save_name); // saves, below
bool valid_saved_game_name(LPCSTR file_name); // console_commands.cpp

game_sv_Coop::game_sv_Coop() : m_bootstrap_reported(false), m_loaded_save(false), m_autosave_ms(0), m_last_autosave(0),
    m_changing_level(false), m_level_change_radius(25.f), m_level_change_notice(0),
    m_bleedout_ms(120000), m_revive_ms(5000), m_revive_range(2.5f), m_revive_health(0.3f), m_down_health(0.05f),
    m_item_states_checked(0)
{
    m_type = eGameIDCoop;
    m_server_probe = !!strstr(Core.Params, "-coop_server_probe");
    m_probe_stopping = false;
    m_probe_started = m_probe_last_report = m_probe_ticks = 0;
    m_probe_duration = 60000;
    m_probe_body = u16(-1);
    m_probe_body_created = m_probe_body_released = false;
    m_probe_routing_tested = false;
}

void game_sv_Coop::Create(shared_str& options)
{
    R_ASSERT2(!g_dedicated_server, "COOP_BOOTSTRAP requires full runtime; legacy dedicated lacks AI/Lua services");
    R_ASSERT2(strstr(options.c_str(), "/alife"), "COOP_BOOTSTRAP requires /alife/new");
    if (m_server_probe)
    {
        string_path config_path;
        FS.update_path(config_path, "$app_data_root$", "coop_server.ltx");
        CInifile config(config_path);
        // run_seconds = 0: no timed stop (a play session; the console quit remains).
        u32 seconds = config.r_u32("server", "run_seconds");
        R_ASSERT2(seconds == 0 || (seconds >= 10 && seconds <= 3600), "COOP_SERVER run_seconds must be 0 or 10..3600");
        m_probe_duration = seconds * 1000;
        const u32 autosave_minutes = config.line_exist("server", "autosave_minutes") ? config.r_u32("server", "autosave_minutes") : 10;
        m_autosave_ms = autosave_minutes * 60 * 1000;
        m_last_autosave = Device.dwTimeGlobal;
        if (config.line_exist("server", "level_change_radius")) m_level_change_radius = config.r_float("server", "level_change_radius");
        if (config.line_exist("server", "bleedout_seconds")) m_bleedout_ms = config.r_u32("server", "bleedout_seconds") * 1000;
        if (config.line_exist("server", "revive_seconds")) m_revive_ms = config.r_u32("server", "revive_seconds") * 1000;
        if (config.line_exist("server", "revive_range")) m_revive_range = config.r_float("server", "revive_range");
        if (config.line_exist("server", "revive_health")) m_revive_health = config.r_float("server", "revive_health");
    }
    Msg("[COOP_BOOTSTRAP] creating world; admission=%s",
        strstr(Core.Params, "-coop_server_network_test") ?
        (strstr(Core.Params, "-coop_server_lan_test") ? "private LAN test (2 clients)" : "loopback test") : "disabled");
    // CALifeSimulator rebuilds the options as <game>/<type>/alife (as in single player); the port
    // token of -coop_port (portsv=N) must survive that for IPureServer::Connect, which binds after Create.
    string64 port_token = "";
    if (LPCSTR token = strstr(options.c_str(), "/portsv="))
    {
        LPCSTR token_end = strchr(token + 1, '/');
        strncpy_s(port_token, token, token_end ? size_t(token_end - token) : xr_strlen(token));
    }
    inherited::Create(options);
    if (port_token[0] && !strstr(options.c_str(), "/portsv="))
    {
        string512 with_port;
        strconcat(sizeof(with_port), with_port, options.c_str(), port_token);
        options = with_port;
    }
    R_ASSERT2(m_alife_simulator && alife().graph().actor(), "COOP_BOOTSTRAP world actor missing");
    alife().graph().actor()->CSE_ALifeObject::can_switch_offline(false);
    Msg("[COOP_BOOTSTRAP] ALife created; world_actor=%u level=%s",
        alife().graph().actor()->ID, alife().level_name().c_str());
    // A world from a save (start server(<name>/coop/alife/load), or <name>/coop/alife after the
    // console's "load": the storage manager loads the file when it exists): the players' bodies
    // are in it, named by their connections (PrepareClient). They wait offline like parked ones,
    // whatever they were doing when the game was saved; a player of that name gets theirs back.
    bool loaded = false;
    if (xr_strcmp(g_pGamePersistent->m_game_params.m_new_or_load, "new"))
    {
        string_path save_name, save_file;
        strconcat(sizeof(save_name), save_name, g_pGamePersistent->m_game_params.m_game_or_spawn, SAVE_EXTENSION);
        FS.update_path(save_file, "$game_saves$", save_name);
        loaded = !!FS.exist(save_file);
    }
    if (loaded)
    {
        m_loaded_save = true;
        const CSE_ALifeCreatureActor* world = alife().graph().actor();
        const CALifeSimulator& simulator = alife(); // the const accessors are the public ones
        u32 bodies = 0;
        for (CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it = simulator.objects().objects().begin();
             it != simulator.objects().objects().end(); ++it)
        {
            CSE_ALifeCreatureActor* body = smart_cast<CSE_ALifeCreatureActor*>(it->second);
            if (!body || body == world) continue;
            body->CSE_ALifeObject::can_switch_online(false);
            body->CSE_ALifeObject::can_switch_offline(true);
            // The saved actor state (ClientSave or the parked copy written by save_game) is taken
            // before the switch manager's first pass clears it from the offline entity.
            // The connection name has '_' where the shown character name has spaces (SpawnBody).
            string256 key;
            xr_strcpy(key, body->m_character_name_str.c_str() ? body->m_character_name_str.c_str() : "");
            for (char* c = key; *c; ++c) if (*c == ' ') *c = '_';
            const shared_str player(key);
            SParkedBody& parked = m_parked[player];
            parked.id = body->ID;
            parked.parked_at = Device.dwTimeGlobal;
            parked.state = body->client_data;
            parked.items.clear();
            for (u32 i = 0; i < body->children.size(); ++i)
            {
                CSE_ALifeDynamicObject* item = simulator.objects().object(body->children[i], true);
                if (item && !item->client_data.empty()) parked.items[item->ID] = item->client_data;
            }
            ++bodies;
            Msg("[COOP_SERVER] LOADED_BODY id=%u name=%s player=%s online=%d alive=%d state=%u items=%u", body->ID, body->name_replace(),
                player.c_str(), body->m_bOnline, body->g_Alive(), u32(parked.state.size()), u32(parked.items.size()));
        }
        coop_store_load(m_store, g_pGamePersistent->m_game_params.m_game_or_spawn);
        Msg("[COOP_SERVER] LOADED_SAVE name=%s bodies=%u", g_pGamePersistent->m_game_params.m_game_or_spawn, bodies);
        // The server Lua restores its world state from alife_storage_manager when the world actor spawns.
        ::luabind::functor<void> functor;
        if (ai().script_engine().functor("coop_server_actor.on_world_loaded", functor))
            functor(g_pGamePersistent->m_game_params.m_game_or_spawn);
    }
}

void game_sv_Coop::OnEvent(NET_Packet& packet, u16 type, u32 time, ClientID sender)
{
    if (type == GAME_EVENT_CREATE_CLIENT)
    {
        IClient* client = server().ID_to_client(sender);
        if (!client)
            return;
        const bool internal = client == server().GetServerClient() ||
            (!m_bootstrap_reported && client->process_id == GetCurrentProcessId());
        if (!internal)
        {
            ip_address address;
            const bool has_address = server().GetClientAddress(sender, address, NULL);
            const bool private_peer = has_address && (address.m_data.a1 == 10 ||
                (address.m_data.a1 == 192 && address.m_data.a2 == 168) ||
                (address.m_data.a1 == 172 && address.m_data.a2 >= 16 && address.m_data.a2 <= 31));
            // -coop_server_open: a server the players reach over the internet (port forwarding, a
            // VPN with public-range addresses); the player limit below still holds.
            const bool allowed_peer = has_address && (address.m_data.a1 == 127 ||
                (strstr(Core.Params, "-coop_server_lan_test") && private_peer) ||
                strstr(Core.Params, "-coop_server_open"));
            if (!m_bootstrap_reported || !strstr(Core.Params, "-coop_server_network_test") ||
                !allowed_peer || server().GetClientsCount() > 3)
            {
                Msg("[COOP_SERVER] JOIN_REJECT client=%u peer=%s", sender.value(), has_address ? address.to_string().c_str() : "?");
                server().DisconnectClient(client, "Coop test: peer restricted or two-player limit reached");
                return;
            }
            Msg("[COOP_SERVER] JOIN_ACCEPT client=%u peer=%s pid=%u", sender.value(), address.to_string().c_str(), client->process_id);
        }
        else Msg("[COOP_BOOTSTRAP] admitting internal client=%u", sender.value());
    }
    inherited::OnEvent(packet, type, time, sender);
}

// The saved actor state of a body: the payload CGameObject::net_Save puts into its size16 chunk,
// which is exactly what CSE_Abstract::load keeps as client_data and CGameObject::net_Spawn loads.
static void coop_body_state(CGameObject* body, xr_vector<u8>& state)
{
    state.clear();
    if (!body) return;
    NET_Packet P;
    P.write_start(); // NET_Packet() leaves B.count uninitialised
    body->net_Save(P);
    if (P.B.count < sizeof(u16)) return;
    u16 size;
    CopyMemory(&size, P.B.data, sizeof(u16));
    if (size == 0 || sizeof(u16) + size > P.B.count) return;
    state.assign(P.B.data + sizeof(u16), P.B.data + sizeof(u16) + size);
}

bool game_sv_Coop::ReclaimBody(xrClientData* client)
{
    xr_map<shared_str, SParkedBody>::iterator it = m_parked.find(client->name);
    if (it == m_parked.end()) return false;
    const SParkedBody parked = it->second;
    m_parked.erase(it);
    // Offline objects are not in the server entity map, only in the ALife registry.
    const CALifeSimulator& simulator = alife();
    CSE_ALifeCreatureActor* body = smart_cast<CSE_ALifeCreatureActor*>(simulator.objects().object(parked.id, true));
    CSE_ALifeCreatureActor* world = alife().graph().actor();
    // An offline entity has no owner; Process_spawn hands it to the internal client again.
    if (!body || body == world || body->owner || !body->g_Alive() || body->m_bOnline)
    {
        Msg("[COOP_SERVER] PLAYER_RECLAIM_FAILED client=%u name=%s body=%u", client->ID.value(), client->name.c_str(), parked.id);
        return false;
    }
    // The actor state rides on the spawn (CGameObject::net_Spawn loads client_data); the ALife
    // record already holds position, health and the inventory as offline children.
    body->client_data = parked.state;
    for (xr_map<u16, xr_vector<u8>>::const_iterator it = parked.items.begin(); it != parked.items.end(); ++it)
    {
        CSE_ALifeDynamicObject* item = simulator.objects().object(it->first, true);
        if (item && item->ID_Parent == body->ID && !item->m_bOnline)
            item->client_data = it->second;
    }
    body->CSE_ALifeObject::can_switch_online(true);
    alife().switch_online(body);
    body->CSE_ALifeObject::can_switch_offline(false);
    client->owner = body;
    client->ps->GameID = body->ID;
    Msg("[COOP_SERVER] PLAYER_RECLAIM client=%u name=%s body=%u children=%u state=%u items=%u parked_ms=%u",
        client->ID.value(), client->name.c_str(), body->ID, u32(body->children.size()), u32(parked.state.size()),
        u32(parked.items.size()), Device.dwTimeGlobal - parked.parked_at);
    // The join menu's faction and icon apply to a returning body too (the profile may have changed).
    {
        shared_str faction, icon;
        u32 money = 0;
        COOP_LOADOUT ignored;
        coop_parse_profile(client->coop_profile.c_str(), faction, icon, money, ignored);
        if (faction.size() || icon.size())
        {
            SBodyProfile& profile = m_profiles[body->ID];
            profile.faction = faction;
            profile.icon = icon;
        }
    }
    return true;
}

bool game_sv_Coop::ParkBody(xrClientData* client, CSE_ALifeCreatureActor* body)
{
    if (!body->g_Alive() || !body->m_bOnline || !client->name.size()) return false;
    SParkedBody& parked = m_parked[client->name];
    parked.id = body->ID;
    parked.parked_at = Device.dwTimeGlobal;
    coop_body_state(smart_cast<CGameObject*>(Level().Objects.net_Find(body->ID)), parked.state);
    parked.items.clear();
    for (u32 i = 0; i < body->children.size(); ++i)
    {
        const u16 child = body->children[i];
        xr_vector<u8>& state = parked.items[child];
        coop_body_state(smart_cast<CGameObject*>(Level().Objects.net_Find(child)), state);
        if (state.empty()) parked.items.erase(child);
    }
    // Offline like a distant NPC: no online object, nothing to attack, inventory kept as
    // ALife children. The switch manager must not bring it back for a passing player.
    body->CSE_ALifeObject::can_switch_online(false);
    body->CSE_ALifeObject::can_switch_offline(true);
    alife().switch_offline(body);
    Msg("[COOP_SERVER] PLAYER_PARK client=%u name=%s body=%u children=%u state=%u items=%u health=%f",
        client->ID.value(), client->name.c_str(), body->ID, u32(body->children.size()), u32(parked.state.size()),
        u32(parked.items.size()), body->get_health());
    return true;
}

// ---------------------------------------------------------------------------------------------
// NPC dialogs for player bodies. Mirrors CUITalkWnd (InitOthersStartDialog / UpdateQuestions /
// AskQuestion / SayPhrase) on the server with the body as "our" side and streams the result.
// Anomaly's dialog scripts use db.actor for the player; for a body's dialog it is swapped to the
// body while the scripts may run, and restored afterwards (CoopLuaActor, see game_sv_coop.h).
CoopLuaActor::CoopLuaActor(CGameObject* body, bool scope) : saved_context(game_sv_Coop::s_context_body), active(false), dialog_scope(scope)
{
    if (dialog_scope) ++game_sv_Coop::s_dialog_scope;
    if (!body) return;
    game_sv_Coop::s_context_body = body;
    lua_State* L = ai().script_engine().lua();
    globals = luabind::get_globals(L);
    luabind::object table = globals["db"];
    static u32 probe_time = 0, probe_swaps = 0, probe_failed = 0;
    if (!table.is_valid() || table.type() != LUA_TTABLE)
    {
        ++probe_failed;
        return;
    }
    db = table;
    saved = table["actor"];
    table["actor"] = body->lua_game_object();
    // `who:id() == AC_ID` is how Anomaly asks "is this the actor"; the constant follows db.actor.
    saved_ac_id = globals["AC_ID"];
    globals["AC_ID"] = u32(body->ID());
    active = true;
    ++probe_swaps;
    if (strstr(Core.Params, "-coop_damage_probe") && Device.dwTimeGlobal - probe_time >= 5000)
    {
        probe_time = Device.dwTimeGlobal;
        luabind::object check = table["actor"];
        CScriptGameObject* now = check.is_valid() && check.type() == LUA_TUSERDATA ? luabind::object_cast<CScriptGameObject*>(check) : NULL;
        luabind::object ac_id = globals["AC_ID"];
        Msg("[COOP_ACTOR_CTX] swaps=%u failed=%u body=%u lua_actor=%u ac_id=%u dialog_scope=%u", probe_swaps, probe_failed, body->ID(),
            now ? now->ID() : u16(-1), ac_id.is_valid() && ac_id.type() == LUA_TNUMBER ? luabind::object_cast<u32>(ac_id) : u32(-1),
            dialog_scope ? 1 : 0);
        probe_swaps = probe_failed = 0;
    }
}

CoopLuaActor::~CoopLuaActor()
{
    if (active)
    {
        db["actor"] = saved;
        globals["AC_ID"] = saved_ac_id;
    }
    game_sv_Coop::s_context_body = saved_context;
    if (dialog_scope) --game_sv_Coop::s_dialog_scope;
}

u32 game_sv_Coop::s_dialog_scope = 0;
CGameObject* game_sv_Coop::s_context_body = NULL;

CGameObject* game_sv_Coop::ContextActor(u16 requested_id)
{
    if (!s_context_body || !IsGameTypeCoop() || !OnServer() || !ai().get_alife()) return NULL;
    CSE_ALifeCreatureActor* world = ai().alife().graph().actor();
    if (!world || requested_id != world->ID) return NULL;
    return s_context_body->getDestroy() ? NULL : s_context_body;
}

const CSE_Abstract* game_sv_Coop::s_spatial_context = NULL;

CoopSpatialScope::CoopSpatialScope(const CSE_Abstract* entity) : saved(game_sv_Coop::s_spatial_context)
{
    game_sv_Coop::s_spatial_context = entity;
}

CoopSpatialScope::~CoopSpatialScope()
{
    game_sv_Coop::s_spatial_context = saved;
}

// -coop_no_actor_anchor: off; -coop_actor_anchor_ctx: only in a body or ALife-object context (no world fallback)
static int coop_actor_anchor_mode()
{
    static int mode = -1;
    if (mode < 0) mode = strstr(Core.Params, "-coop_no_actor_anchor") ? 0 : strstr(Core.Params, "-coop_actor_anchor_ctx") ? 1 : 2;
    return mode;
}

static bool coop_actor_anchor(const Fvector& real, Fvector& position)
{
    const int mode = coop_actor_anchor_mode();
    if (mode == 0) return false;
    CGameObject* body = game_sv_Coop::s_context_body;
    if (body && !body->getDestroy())
    {
        position = body->Position();
        return true;
    }
    const CSE_Abstract* context = game_sv_Coop::s_spatial_context;
    if (mode == 1 && !context) return false;
    CActor* nearest = game_sv_Coop::NearestBody(context ? context->o_Position : real);
    if (!nearest) return false;
    position = nearest->Position();
    return true;
}

bool game_sv_Coop::WorldActorAnchor(const CSE_Abstract* entity, Fvector& position)
{
    if (!entity || !IsGameTypeCoop() || !OnServer() || !ai().get_alife()) return false;
    if (entity != ai().alife().graph().actor()) return false;
    return coop_actor_anchor(entity->o_Position, position);
}

bool game_sv_Coop::WorldActorAnchor(const CObject* object, Fvector& position)
{
    if (!object || !IsGameTypeCoop() || !OnServer() || !ai().get_alife()) return false;
    CSE_ALifeCreatureActor* world = ai().alife().graph().actor();
    if (!world || object->ID() != world->ID) return false;
    return coop_actor_anchor(object->Position(), position);
}

// xrServerEntities (cse_abstract.position) cannot see game_sv_Coop
bool coop_world_actor_anchor(const CSE_Abstract* entity, Fvector& position)
{
    return game_sv_Coop::WorldActorAnchor(entity, position);
}

CActor* game_sv_Coop::BodyOf(const CObject* object)
{
    if (!object || !IsGameTypeCoop() || !OnServer() || object == Level().CurrentControlEntity()) return NULL;
    return smart_cast<CActor*>(const_cast<CObject*>(object));
}

CActor* game_sv_Coop::NearestBody(const Fvector& position)
{
    if (!IsGameTypeCoop() || !OnServer() || !Level().Server || Level().Server->game->Type() != eGameIDCoop)
        return NULL;
    // The list of living bodies is rebuilt once per frame; NPC updates ask many times a frame.
    static u32 frame = u32(-1);
    static xr_vector<u16> bodies; // ids, resolved on every call: a body may leave within the frame
    if (frame != Device.dwFrame)
    {
        frame = Device.dwFrame;
        bodies.clear();
        xrServer& server = *Level().Server;
        IClient* internal = server.GetServerClient();
        auto visit = [&](IClient* connection)
        {
            if (connection == internal || !connection->flags.bConnected) return;
            xrClientData* client = static_cast<xrClientData*>(connection);
            if (client->ps && client->owner && client->net_Accepted) bodies.push_back(client->ps->GameID);
        };
        server.ForEachClientDo(visit);
    }
    CActor* best = NULL;
    float best_distance = flt_max;
    for (u32 i = 0; i < bodies.size(); ++i)
    {
        CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(bodies[i]));
        if (!body || !body->g_Alive() || body->getDestroy()) continue;
        const float distance = body->Position().distance_to_sqr(position);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = body;
        }
    }
    return best;
}

// ---- Lua owners: an NPC whose Lua runs for one player, not the nearest one --------------------------
// Companions (axr_companions/axr_beh follow "the actor"): the server Lua names the recruiting player
// (level.coop_set_lua_owner); the binder updates of that NPC then run with the owner's body as
// db.actor while the owner is in the world, with the nearest body otherwise.

static xr_map<u16, shared_str> coop_lua_owners; // object id -> player (connection name)
static xrClientData* coop_client_of(CActor* body); // below, with the level change

void game_sv_Coop::SetLuaOwner(u16 object_id, LPCSTR player)
{
    if (player && xr_strlen(player)) coop_lua_owners[object_id] = player;
    else coop_lua_owners.erase(object_id);
}

CActor* game_sv_Coop::BodyOfPlayer(LPCSTR player)
{
    if (!player || !xr_strlen(player) || !IsGameTypeCoop() || !OnServer() || !Level().Server) return NULL;
    xrServer& server = *Level().Server;
    IClient* internal = server.GetServerClient();
    u16 id = u16(-1);
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* client = static_cast<xrClientData*>(connection);
        if (client->ps && client->owner && client->net_Accepted && !xr_strcmp(client->name.c_str(), player)) id = client->ps->GameID;
    };
    server.ForEachClientDo(visit);
    CActor* body = id != u16(-1) ? smart_cast<CActor*>(Level().Objects.net_Find(id)) : NULL;
    return body && body->g_Alive() && !body->getDestroy() ? body : NULL;
}

LPCSTR game_sv_Coop::PlayerOfBody(u16 body_id)
{
    CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(body_id));
    xrClientData* client = body ? coop_client_of(body) : NULL;
    return client && client->name.size() ? client->name.c_str() : "";
}

CActor* game_sv_Coop::ContextBodyFor(CGameObject* object)
{
    if (!object) return NULL;
    if (!coop_lua_owners.empty())
    {
        xr_map<u16, shared_str>::const_iterator it = coop_lua_owners.find(object->ID());
        if (it != coop_lua_owners.end())
        {
            CActor* owner = BodyOfPlayer(it->second.c_str());
            if (owner) return owner;
        }
    }
    return NearestBody(object->Position());
}

static CActor* coop_talk_body(xrClientData* client)
{
    if (!client || !client->owner || !client->ps) return NULL;
    CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(client->ps->GameID));
    return body && body->g_Alive() ? body : NULL;
}

static void coop_talk_send(xrClientData* client, NET_Packet& P)
{
    Level().Server->SendTo(client->ID, P, net_flags(TRUE, TRUE));
}

void game_sv_Coop::OnTalkMessage(xrClientData* client, NET_Packet& P)
{
    if (P.B.count < P.r_tell() + sizeof(u8)) return;
    const u8 op = P.r_u8();
    if (strstr(Core.Params, "-coop_damage_probe"))
        Msg("[COOP_SERVER] TALK_MSG client=%u op=%u bytes=%u", client->ID.value(), op, P.B.count);
    switch (op)
    {
    case 1:
        if (P.B.count >= P.r_tell() + sizeof(u16)) TalkStart(client, P.r_u16());
        break;
    case 2:
    {
        shared_str id;
        P.r_stringZ(id);
        TalkSelect(client, id);
        break;
    }
    case 3:
        TalkStop(client, false);
        break;
    }
}

void game_sv_Coop::TalkAnswer(xrClientData* client, bool ours, LPCSTR text)
{
    if (!text || !text[0]) return;
    NET_Packet P;
    P.w_begin(M_COOP_TALK);
    P.w_u8(2);
    P.w_u8(ours ? 1 : 0);
    P.w_stringZ(text);
    coop_talk_send(client, P);

    // The other players follow the conversation as game news: the speaker's name and portrait
    // (the NPC's, or the talking player's body) with the phrase, like a dynamic news entry.
    xr_map<u32, SCoopTalk>::const_iterator talk = m_talks.find(client->ID.value());
    LPCSTR speaker = client->name.c_str();
    LPCSTR icon = "";
    CInventoryOwner* face = NULL;
    if (!ours && talk != m_talks.end())
    {
        face = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(talk->second.npc));
        if (face) speaker = face->Name();
    }
    else if (client->owner)
        face = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(client->owner->ID));
    if (face && face->IconName()) icon = face->IconName();
    NET_Packet observe;
    observe.w_begin(M_COOP_TALK);
    observe.w_u8(5);
    observe.w_stringZ(speaker);
    observe.w_stringZ(text);
    observe.w_stringZ(icon);
    IClient* internal = server().GetServerClient();
    xr_vector<ClientID> observers; // sent after the iteration: the client lock is not recursive
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || connection == client || !connection->flags.bConnected) return;
        xrClientData* other = static_cast<xrClientData*>(connection);
        if (other->net_Accepted && other->owner) observers.push_back(other->ID);
    };
    server().ForEachClientDo(visit);
    for (u32 i = 0; i < observers.size(); ++i)
        server().SendTo(observers[i], observe, net_flags(TRUE, TRUE));
}

void game_sv_Coop::TalkNpcAnswer(CGameObject* listener, LPCSTR text)
{
    if (!Level().Server || Level().Server->game->Type() != eGameIDCoop) return;
    game_sv_Coop* self = static_cast<game_sv_Coop*>(Level().Server->game);
    CSE_Abstract* body = Level().Server->ID_to_entity(listener->ID());
    xrClientData* client = Level().Server->CoopControllerOf(body);
    if (client && self->m_talks.find(client->ID.value()) != self->m_talks.end())
        self->TalkAnswer(client, false, text);
}

// pda.actor_menu_mode 10/11 of SP (the talk window showing and hiding, InventoryUtilities::
// SendInfoToLuaScripts): the NPC the dialog's Lua addresses through get_speaker (used_npc_id;
// Sidorovich and the Forester have no use_callback that would set it - tasks_fetch indexed a table
// by nil) and actor_on_leave_dialog at the end. The event table is one for the whole VM, so the
// speaker is set again before every phrase: two players in dialogs at once each get their own.
static void coop_talk_speaker(u16 npc_id)
{
    ::luabind::functor<void> set_event;
    if (ai().script_engine().functor("SetEvent", set_event)) set_event("used_npc_id", u32(npc_id));
}

static void coop_talk_left(CActor* body, u16 npc_id)
{
    if (!body) return;
    CoopLuaActor context(body, false);
    ::luabind::functor<void> callback;
    if (ai().script_engine().functor("SendScriptCallback", callback)) callback("actor_on_leave_dialog", u32(npc_id));
}

void game_sv_Coop::TalkStart(xrClientData* client, u16 npc_id)
{
    if (m_talks.find(client->ID.value()) != m_talks.end()) TalkStop(client, false);
    CActor* body = coop_talk_body(client);
    CGameObject* npc_object = smart_cast<CGameObject*>(Level().Objects.net_Find(npc_id));
    CInventoryOwner* npc = smart_cast<CInventoryOwner*>(npc_object);
    CEntityAlive* npc_alive = smart_cast<CEntityAlive*>(npc_object);
    CPhraseDialogManager* npc_dialogs = smart_cast<CPhraseDialogManager*>(npc_object);
    if (!body || !npc || !npc_alive || !npc_dialogs || body->IsTalking() || npc_alive == body ||
        !npc_alive->g_Alive() || npc->IsTalking() || smart_cast<CActor*>(npc_object) ||
        body->Position().distance_to(npc_object->Position()) > 4.f)
    {
        Msg("[COOP_SERVER] TALK_REJECT client=%u npc=%u", client->ID.value(), npc_id);
        return;
    }
    CoopLuaActor lua(body);
    if (npc->IsTalkEnabled())
    {
        if (!npc->OfferTalk(body)) return; // both alive; NPC side starts talking
    }
    else
    {
        // xr_meet enables talking by the world actor's distance and sight, which never holds for
        // a body; hostile NPCs still refuse, everyone else talks the way they do in SP when met.
        const ALife::ERelationType relation = RELATION_REGISTRY().GetRelationType(npc, static_cast<CInventoryOwner*>(body));
        if (relation == ALife::eRelationTypeEnemy)
        {
            Msg("[COOP_SERVER] TALK_REJECT client=%u npc=%u reason=enemy", client->ID.value(), npc_id);
            return;
        }
        npc->StartTalk(body);
    }
    body->StartTalk(npc);
    coop_talk_speaker(npc_id);
    SCoopTalk& talk = m_talks[client->ID.value()];
    talk.npc = npc_id;
    talk.dialog = DIALOG_SHARED_PTR((CPhraseDialog*)NULL);
    NET_Packet P;
    P.w_begin(M_COOP_TALK);
    P.w_u8(1);
    P.w_u16(npc_id);
    P.w_u8(npc->bDisableBreakDialog ? 1 : 0);
    coop_talk_send(client, P);
    Msg("[COOP_SERVER] TALK_START client=%u body=%u npc=%u", client->ID.value(), body->ID(), npc_id);
    // CUITalkWnd::InitOthersStartDialog
    npc_dialogs->UpdateAvailableDialogs(body);
    if (!npc_dialogs->AvailableDialogs().empty())
    {
        talk.dialog = npc_dialogs->AvailableDialogs().front();
        npc_dialogs->InitDialog(body, talk.dialog);
        TalkAnswer(client, false, talk.dialog->GetPhraseText("0"));
        npc_dialogs->SayPhrase(talk.dialog, "0");
        if (!talk.dialog || talk.dialog->IsFinished()) talk.dialog = DIALOG_SHARED_PTR((CPhraseDialog*)NULL);
    }
    TalkSendQuestions(client, talk);
}

void game_sv_Coop::TalkSay(xrClientData* client, SCoopTalk& talk, const shared_str& phrase_id)
{
    CActor* body = coop_talk_body(client);
    if (!body || !talk.dialog) return;
    TalkAnswer(client, true, talk.dialog->GetPhraseText(phrase_id));
    body->SayPhrase(talk.dialog, phrase_id); // the NPC replies through TalkNpcAnswer
    if (talk.dialog->IsFinished()) talk.dialog = DIALOG_SHARED_PTR((CPhraseDialog*)NULL);
}

void game_sv_Coop::TalkSendQuestions(xrClientData* client, SCoopTalk& talk)
{
    CActor* body = coop_talk_body(client);
    CGameObject* npc_object = smart_cast<CGameObject*>(Level().Objects.net_Find(talk.npc));
    CPhraseDialogManager* npc_dialogs = smart_cast<CPhraseDialogManager*>(npc_object);
    if (!body || !npc_dialogs) return;
    // CUITalkWnd::UpdateQuestions, including its dummy-phrase auto answers
    for (u32 guard = 0; guard < 8; ++guard)
    {
        NET_Packet P;
        P.w_begin(M_COOP_TALK);
        P.w_u8(3);
        if (!talk.dialog)
        {
            body->UpdateAvailableDialogs(npc_dialogs);
            const CPhraseDialogManager::DIALOG_VECTOR& dialogs = body->AvailableDialogs();
            P.w_u8(1);
            P.w_u8(u8(dialogs.size() < 255 ? dialogs.size() : 255));
            for (u32 i = 0; i < dialogs.size() && i < 255; ++i)
            {
                const DIALOG_SHARED_PTR& dialog = dialogs[i];
                P.w_stringZ(dialog->DialogCaption());
                P.w_stringZ(dialog->GetDialogID().c_str());
                P.w_u8(dialog->GetPhrase("0")->IsFinalizer() ? 1 : 0);
            }
            coop_talk_send(client, P);
            return;
        }
        if (talk.dialog->IsWeSpeaking(body))
        {
            const PHRASE_VECTOR& phrases = talk.dialog->PhraseList();
            if (!phrases.empty() && talk.dialog->allIsDummy())
            {
                CPhrase* phrase = phrases[Random.randI(phrases.size())];
                TalkSay(client, talk, phrase->GetID());
                continue;
            }
            if (!phrases.empty())
            {
                P.w_u8(0);
                P.w_u8(u8(phrases.size() < 255 ? phrases.size() : 255));
                for (u32 i = 0; i < phrases.size() && i < 255; ++i)
                {
                    CPhrase* phrase = phrases[i];
                    P.w_stringZ(talk.dialog->GetPhraseText(phrase->GetID()));
                    P.w_stringZ(phrase->GetID().c_str());
                    P.w_u8(phrase->IsFinalizer() ? 1 : 0);
                }
                coop_talk_send(client, P);
                return;
            }
        }
        // The NPC's turn with no phrase to show yet: an empty list keeps the window quiet.
        P.w_u8(0);
        P.w_u8(0);
        coop_talk_send(client, P);
        return;
    }
}

void game_sv_Coop::TalkSelect(xrClientData* client, const shared_str& id)
{
    xr_map<u32, SCoopTalk>::iterator it = m_talks.find(client->ID.value());
    CActor* body = coop_talk_body(client);
    if (it == m_talks.end() || !body || !body->IsTalking()) return;
    SCoopTalk& talk = it->second;
    CGameObject* npc_object = smart_cast<CGameObject*>(Level().Objects.net_Find(talk.npc));
    CPhraseDialogManager* npc_dialogs = smart_cast<CPhraseDialogManager*>(npc_object);
    if (!npc_dialogs) return;
    CoopLuaActor lua(body);
    coop_talk_speaker(talk.npc);
    if (!talk.dialog)
    {
        if (!body->HaveAvailableDialog(id)) return;
        talk.dialog = body->GetDialogByID(id);
        body->InitDialog(npc_dialogs, talk.dialog);
        TalkSay(client, talk, "0");
    }
    else
    {
        if (!talk.dialog->IsWeSpeaking(body)) return;
        bool listed = false;
        const PHRASE_VECTOR& phrases = talk.dialog->PhraseList();
        for (u32 i = 0; i < phrases.size(); ++i)
            if (phrases[i]->GetID() == id) listed = true;
        if (!listed) return;
        TalkSay(client, talk, id);
    }
    TalkSendQuestions(client, talk);
}

void game_sv_Coop::TalkStop(xrClientData* client, bool notify)
{
    TradeStop(client); // a trade lives inside the conversation
    xr_map<u32, SCoopTalk>::iterator it = m_talks.find(client->ID.value());
    if (it == m_talks.end()) return;
    CActor* body = client->ps ? smart_cast<CActor*>(Level().Objects.net_Find(client->ps->GameID)) : NULL;
    CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(it->second.npc));
    if (body && body->IsTalking()) body->StopTalk();
    if (npc && npc->IsTalking()) npc->StopTalk();
    coop_talk_left(body, it->second.npc);
    m_talks.erase(it);
    if (notify)
    {
        NET_Packet P;
        P.w_begin(M_COOP_TALK);
        P.w_u8(4);
        coop_talk_send(client, P);
    }
    Msg("[COOP_SERVER] TALK_STOP client=%u notify=%u", client->ID.value(), notify ? 1 : 0);
}

void game_sv_Coop::TalkUpdate()
{
    if (m_talks.empty()) return;
    xr_vector<u32> ended;
    for (xr_map<u32, SCoopTalk>::iterator it = m_talks.begin(); it != m_talks.end(); ++it)
    {
        ClientID id;
        id.set(it->first);
        xrClientData* client = server().ID_to_client(id);
        CActor* body = coop_talk_body(client);
        CEntityAlive* npc = smart_cast<CEntityAlive*>(Level().Objects.net_Find(it->second.npc));
        if (!client || !body || !body->IsTalking() || !npc || !npc->g_Alive())
            ended.push_back(it->first);
    }
    for (u32 i = 0; i < ended.size(); ++i)
    {
        ClientID id;
        id.set(ended[i]);
        xrClientData* client = server().ID_to_client(id);
        if (client) TalkStop(client, true);
        else m_talks.erase(ended[i]);
    }
}

void game_sv_Coop::OnPlayerStore(xrClientData* client, NET_Packet& P)
{
    if (!client->name.size()) return;
    xr_string blob;
    if (!m_store_parts[client->name].receive(P, blob)) return;
    m_store[client->name].swap(blob);
    if (strstr(Core.Params, "-coop_damage_probe") || strstr(Core.Params, "-coop_store_probe"))
        Msg("[COOP_SERVER] PLAYER_STORE client=%u name=%s bytes=%u", client->ID.value(), client->name.c_str(), u32(m_store[client->name].size()));
}

// Sent before the connection data (xrServer::OnCL_Connected): the client applies it before its
// body spawns, the order of load_state and the actor's net_spawn in SP.
void game_sv_Coop::SendPlayerStore(xrClientData* client)
{
    xr_map<shared_str, xr_string>::const_iterator it = m_store.find(client->name);
    if (it == m_store.end() || it->second.empty()) return;
    const xr_string& blob = it->second;
    const u32 total = u32(blob.size());
    for (u32 offset = 0; offset < total;)
    {
        const u32 length = _min(total - offset, coop_store_part);
        NET_Packet P;
        P.w_begin(M_COOP_PLAYER_STORE);
        coop_store_write_part(P, blob.c_str(), total, offset, length);
        server().SendTo(client->ID, P, net_flags(TRUE, TRUE));
        offset += length;
    }
    Msg("[COOP_SERVER] PLAYER_STORE_SENT client=%u name=%s bytes=%u", client->ID.value(), client->name.c_str(), total);
}

// ---- saves ---------------------------------------------------------------------------------------

static game_sv_Coop* coop_server_game()
{
    if (!IsGameTypeCoop() || !OnServer() || !Level().Server || Level().Server->game->Type() != eGameIDCoop) return NULL;
    return static_cast<game_sv_Coop*>(Level().Server->game);
}

// The per-connection store next to the ALife save: one "name\nblob\n" pair per player (the blobs
// are Lua source without line breaks, see coop_client_actor.store_send).
static void coop_store_path(string_path& path, LPCSTR save_name)
{
    string_path file;
    strconcat(sizeof(file), file, save_name, ".coopstore");
    FS.update_path(path, "$game_saves$", file);
}

static void coop_store_save(const xr_map<shared_str, xr_string>& store, LPCSTR save_name)
{
    string_path path;
    coop_store_path(path, save_name);
    IWriter* writer = FS.w_open(path);
    if (!writer) return;
    for (xr_map<shared_str, xr_string>::const_iterator it = store.begin(); it != store.end(); ++it)
    {
        if (it->second.empty()) continue;
        writer->w(it->first.c_str(), it->first.size());
        writer->w("\n", 1);
        writer->w(it->second.c_str(), u32(it->second.size()));
        writer->w("\n", 1);
    }
    FS.w_close(writer);
}

static void coop_store_load(xr_map<shared_str, xr_string>& store, LPCSTR save_name)
{
    string_path path;
    coop_store_path(path, save_name);
    if (!FS.exist(path)) return;
    IReader* reader = FS.r_open(path);
    if (!reader) return;
    string4096 name;
    xr_string blob;
    u32 loaded = 0;
    while (!reader->eof())
    {
        reader->r_string(name, sizeof(name));
        if (reader->eof()) break;
        reader->r_string(blob);
        if (!name[0]) continue;
        store[shared_str(name)] = blob;
        ++loaded;
    }
    FS.r_close(reader);
    Msg("[COOP_SERVER] PLAYER_STORE_LOADED save=%s players=%u", save_name, loaded);
}

// CLevel::ClientSave without the network: in SP the client and the server share the process and
// the M_SAVE_PACKET states arrive before the file is written; here the internal client talks to
// the server through the loopback transport, so the online objects' states are put on their
// entities directly (the same net_Save payload Process_save stores as client_data).
static u32 coop_save_online_objects()
{
    u32 saved = 0;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
        if (!object || object->getDestroy() || !object->net_SaveRelevant()) continue;
        CSE_Abstract* entity = Level().Server->ID_to_entity(object->ID());
        if (!entity) continue;
        xr_vector<u8> state;
        coop_body_state(object, state);
        entity->client_data = state;
        ++saved;
    }
    return saved;
}

void game_sv_Coop::save_game(NET_Packet& net_packet, ClientID sender)
{
    if (!ai().get_alife()) return;
    shared_str name;
    net_packet.r_stringZ(name);
    const bool update_name = !!net_packet.r_u8();
    if (!name.size() || !valid_saved_game_name(name.c_str()))
    {
        Msg("! [COOP_SERVER] SAVE_REJECT by=%u name=%s", sender.value(), name.c_str() ? name.c_str() : "");
        return;
    }
    const u32 online = coop_save_online_objects();
    // A parked body's state is kept here (offline entities lose theirs on every switch attempt)
    // and goes onto the entities for the file.
    const CALifeSimulator& simulator = alife();
    for (xr_map<shared_str, SParkedBody>::const_iterator it = m_parked.begin(); it != m_parked.end(); ++it)
    {
        CSE_ALifeDynamicObject* body = simulator.objects().object(it->second.id, true);
        if (!body || body->m_bOnline) continue;
        body->client_data = it->second.state;
        for (xr_map<u16, xr_vector<u8>>::const_iterator item = it->second.items.begin(); item != it->second.items.end(); ++item)
        {
            CSE_ALifeDynamicObject* object = simulator.objects().object(item->first, true);
            if (object && object->ID_Parent == body->ID && !object->m_bOnline) object->client_data = item->second;
        }
    }
    alife().save(name.c_str(), update_name); // header, time, spawns, objects, registries + the Lua state (.scoc)
    coop_store_save(m_store, name.c_str());
    {
        u32 upgraded = 0, upgrades = 0;
        for (CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it = simulator.objects().objects().begin();
             it != simulator.objects().objects().end(); ++it)
        {
            CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(it->second);
            if (item && !item->m_upgrades.empty()) { ++upgraded; upgrades += u32(item->m_upgrades.size()); }
        }
        Msg("[COOP_SERVER] SAVE_UPGRADES items=%u upgrades=%u", upgraded, upgrades);
    }
    Msg("[COOP_SERVER] SAVED name=%s by=%u online=%u parked=%u store=%u", name.c_str(), sender.value(), online, u32(m_parked.size()), u32(m_store.size()));
    SendLua(u16(-1), (xr_string("saved|") + name.c_str()).c_str());
}

bool game_sv_Coop::IsLoadedSave()
{
    game_sv_Coop* game = coop_server_game();
    return game && game->m_loaded_save;
}

// ---- level change --------------------------------------------------------------------------------

static xrClientData* coop_client_of(CActor* body)
{
    if (!body) return NULL;
    CSE_Abstract* entity = Level().Server->ID_to_entity(body->ID());
    return entity ? Level().Server->CoopControllerOf(entity) : NULL;
}

void game_sv_Coop::LevelChangeInvite(CActor* body, NET_Packet& change, bool enabled, const shared_str& invite,
                                     bool has_reject, const Fvector& reject_position, const Fvector& reject_angles)
{
    xrClientData* client = coop_client_of(body);
    if (!client || !coop_server_game()) return;
    // The change payload as the dialog will send it back, then the dialog's own texts.
    u16 dummy;
    change.r_begin(dummy);
    GameGraph::_GRAPH_ID game_vertex;
    u32 level_vertex;
    Fvector position, angles;
    change.r(&game_vertex, sizeof(game_vertex));
    change.r(&level_vertex, sizeof(level_vertex));
    change.r_vec3(position);
    change.r_vec3(angles);
    NET_Packet P;
    CGameObject::u_EventGen(P, GE_COOP_LEVEL_INVITE, body->ID());
    P.w_u16(u16(game_vertex));
    P.w_u32(level_vertex);
    P.w_vec3(position);
    P.w_vec3(angles);
    P.w_u8(enabled ? 1 : 0);
    P.w_stringZ(invite.c_str() ? invite.c_str() : "");
    P.w_u8(has_reject ? 1 : 0);
    P.w_vec3(reject_position);
    P.w_vec3(reject_angles);
    Level().Server->SendTo(client->ID, P, net_flags(TRUE, TRUE));
}

void game_sv_Coop::LevelChangeRequest(CActor* body, NET_Packet& change)
{
    xrClientData* client = coop_client_of(body);
    if (!client || !coop_server_game()) return;
    // As if the player's dialog had said yes: through the server's message path, which
    // broadcasts M_CHANGE_LEVEL when the change is accepted.
    Level().Server->OnMessage(change, client->ID);
}

bool game_sv_Coop::change_level(NET_Packet& net_packet, ClientID sender)
{
    if (!ai().get_alife() || m_changing_level) return false;
    xrClientData* client = server().ID_to_client(sender);
    CActor* requester = client && client->ps ? smart_cast<CActor*>(Level().Objects.net_Find(client->ps->GameID)) : NULL;
    if (!requester || !requester->g_Alive())
    {
        Msg("! [COOP_SERVER] LEVEL_CHANGE_REJECT by=%u (no living body)", sender.value());
        return false;
    }
    GameGraph::_GRAPH_ID game_vertex;
    u32 level_vertex;
    Fvector position, angles;
    net_packet.r(&game_vertex, sizeof(game_vertex));
    net_packet.r(&level_vertex, sizeof(level_vertex));
    net_packet.r_vec3(position);
    net_packet.r_vec3(angles);
    if (!ai().game_graph().valid_vertex_id(game_vertex))
    {
        Msg("! [COOP_SERVER] LEVEL_CHANGE_REJECT by=%u (bad vertex %u)", sender.value(), u32(game_vertex));
        return false;
    }
    // Everyone travels together: the other living players must be with the requester.
    xr_vector<CActor*> bodies;
    u32 far_away = 0;
    IClient* internal = server().GetServerClient();
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* data = static_cast<xrClientData*>(connection);
        if (!data->net_Accepted || !data->owner || !data->ps) return;
        CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(data->ps->GameID));
        if (!body || !body->g_Alive()) return;
        bodies.push_back(body);
        if (body != requester && body->Position().distance_to(requester->Position()) > m_level_change_radius) ++far_away;
    };
    server().ForEachClientDo(visit);
    if (far_away)
    {
        if (Device.dwTimeGlobal - m_level_change_notice > 4000)
        {
            m_level_change_notice = Device.dwTimeGlobal;
            string256 text;
            xr_sprintf(text, "actor_msg|1|Waiting for the others at the exit: %u of %u|4", u32(bodies.size() - far_away), u32(bodies.size()));
            SendLua(requester->ID(), text);
        }
        Msg("[COOP_SERVER] LEVEL_CHANGE_WAIT by=%u body=%u far=%u of %u", sender.value(), requester->ID(), far_away, u32(bodies.size()));
        return false;
    }
    m_changing_level = true;
    const CGameGraph::CVertex* vertex = ai().game_graph().vertex(game_vertex);
    LPCSTR level_name = *ai().game_graph().header().level(vertex->level_id()).name();
    Msg("[COOP_SERVER] LEVEL_CHANGE by=%u to=%s vertex=%u bodies=%u parked=%u", sender.value(), level_name, u32(game_vertex),
        u32(bodies.size()), u32(m_parked.size()));
    SendLua(u16(-1), (xr_string("levelchange|") + level_name).c_str());
    // The bodies' states before they leave the level (teleport_object switches them offline, which
    // clears an entity's client data), then the move: each body a step apart around the entrance.
    // The items' states as well: the offline switch clears the children's client data too
    // (CSE_ALifeTraderAbstract::add_offline), and an item without it spawns on the new level with
    // no place - the weapons, outfit and PDA fell into the ruck by default_to_ruck. As ParkBody.
    xr_vector<std::pair<u16, xr_vector<u8>>> states;
    xr_map<u16, xr_vector<u8>> item_states;
    for (u32 i = 0; i < bodies.size(); ++i)
    {
        xr_vector<u8> state;
        coop_body_state(bodies[i], state);
        states.push_back(std::make_pair(bodies[i]->ID(), state));
        CSE_Abstract* entity = server().ID_to_entity(bodies[i]->ID());
        if (!entity) continue;
        for (u32 c = 0; c < entity->children.size(); ++c)
        {
            const u16 child = entity->children[c];
            xr_vector<u8> item_state;
            coop_body_state(smart_cast<CGameObject*>(Level().Objects.net_Find(child)), item_state);
            if (!item_state.empty()) item_states[child] = item_state;
        }
    }
    xr_vector<u16> ids;
    for (u32 i = 0; i < bodies.size(); ++i) ids.push_back(bodies[i]->ID());
    for (xr_map<shared_str, SParkedBody>::const_iterator it = m_parked.begin(); it != m_parked.end(); ++it) ids.push_back(it->second.id);
    const CALifeSimulator& simulator = alife();
    // The bodies stand in a line from the entrance point into the level, a metre apart, along the
    // direction the changer faces them (the actor's forward for its yaw, Actor_Movement.cpp:
    // rotateY(-yaw)). A ring around the point put a body outside the level at the Cordon south
    // entrance, behind its boundary; forward is the way the players are meant to walk.
    Fmatrix facing;
    facing.rotateY(-angles.y);
    Fvector forward = facing.k;
    forward.y = 0.f;
    forward.normalize_safe();
    for (u32 i = 0; i < ids.size(); ++i)
    {
        Fvector where = position;
        where.mad(forward, 1.f * float(i));
        alife().teleport_object(ids[i], game_vertex, level_vertex, where);
        CSE_ALifeDynamicObject* entity = simulator.objects().object(ids[i], true);
        if (!entity) continue;
        entity->o_Angle = angles;
        // As CALifeUpdateManager::change_level does for the actor: the body arrives looking the
        // changer's way instead of keeping the torso of the level it left.
        CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(entity);
        if (creature)
        {
            creature->o_torso.yaw = angles.y;
            creature->o_torso.pitch = angles.x;
            creature->o_torso.roll = 0.f;
        }
        Msg("[COOP_SERVER] LEVEL_CHANGE_PLACE body=%u position=%f,%f,%f", entity->ID, VPUSH(where));
        for (u32 s = 0; s < states.size(); ++s)
            if (states[s].first == ids[i]) entity->client_data = states[s].second;
        u32 restored = 0;
        for (xr_map<u16, xr_vector<u8>>::const_iterator item = item_states.begin(); item != item_states.end(); ++item)
        {
            CSE_ALifeDynamicObject* object = simulator.objects().object(item->first, true);
            if (!object || object->ID_Parent != entity->ID) continue;
            object->client_data = item->second;
            ++restored;
        }
        if (restored) Msg("[COOP_SERVER] LEVEL_CHANGE_ITEMS body=%u items=%u", entity->ID, restored);
        for (xr_map<shared_str, SParkedBody>::const_iterator it = m_parked.begin(); it != m_parked.end(); ++it)
        {
            if (it->second.id != ids[i]) continue;
            entity->client_data = it->second.state;
            for (xr_map<u16, xr_vector<u8>>::const_iterator item = it->second.items.begin(); item != it->second.items.end(); ++item)
            {
                CSE_ALifeDynamicObject* object = simulator.objects().object(item->first, true);
                if (object && object->ID_Parent == entity->ID) object->client_data = item->second;
            }
        }
    }
    // The world actor anchors the level: it goes too — its fields only, as SP does before its
    // autosave (the registry is rebuilt by the restart; the live object stays until then).
    CSE_ALifeCreatureActor* world = alife().graph().actor();
    world->m_tGraphID = game_vertex;
    world->m_tNodeID = level_vertex;
    world->o_Position = position;
    world->o_Angle = angles;
    world->o_torso.yaw = angles.y;
    world->o_torso.pitch = angles.x;
    world->o_torso.roll = 0.f;
    // The world Lua's level change (SP: on_level_changing from the actor's save): companion squads
    // move to the world actor's new vertex — the destination, set just above.
    {
        ::luabind::functor<void> functor;
        if (ai().script_engine().functor("coop_server_actor.on_level_change", functor)) functor(level_name);
    }
    // The world as it is now, on the new level, is the world the restart loads.
    const u32 online = coop_save_online_objects();
    alife().save("coop_level_change", true);
    coop_store_save(m_store, "coop_level_change");
    {
        // As CALifeUpdateManager::change_level: the restart's server options name the save.
        shared_str* options = alife().server_command_line();
        LPCSTR rest = options->c_str() ? strstr(options->c_str(), "/") : NULL;
        string512 rewritten;
        strconcat(sizeof(rewritten), rewritten, "coop_level_change", rest ? rest : "/coop/alife");
        *options = rewritten;
    }
    Msg("[COOP_SERVER] LEVEL_CHANGE_SAVED to=%s online=%u moved=%u", level_name, online, u32(ids.size()));
    return true; // xrServer broadcasts M_CHANGE_LEVEL: the internal client reconnects, the players' clients drop and return
}

bool game_sv_Coop::load_game(NET_Packet& net_packet, ClientID sender)
{
    if (!server().GetServerClient() || sender != server().GetServerClient()->ID)
    {
        Msg("! [COOP_SERVER] LOAD_REJECT by=%u (server console only)", sender.value());
        return false;
    }
    const u32 cursor = net_packet.r_tell();
    shared_str name;
    net_packet.r_stringZ(name);
    net_packet.r_seek(cursor);
    // The players see the notice before the world goes away (M_LOAD_GAME then makes the
    // internal client reconnect: a full restart, Create runs again with the save).
    SendLua(u16(-1), (xr_string("reload|") + name.c_str()).c_str());
    const bool ok = inherited::load_game(net_packet, sender); // rewrites the server options to <name>/coop/alife
    Msg("[COOP_SERVER] LOAD name=%s ok=%d clients=%u", name.c_str(), ok ? 1 : 0, server().GetClientsCount());
    return ok;
}

// ---- player death: downed, revive, bleed-out, respawn ----------------------------------------------

static LPCSTR coop_body_player(CActor* body)
{
    xrClientData* client = coop_client_of(body);
    return client && client->name.size() ? client->name.c_str() : "?";
}

bool game_sv_Coop::IsDowned(u16 body_id)
{
    game_sv_Coop* game = coop_server_game();
    return game && game->m_downed.find(body_id) != game->m_downed.end();
}

// CEntity::KillEntity on the server: a player's body about to die goes down instead, unless its
// bleed-out already ran out (KillDowned kills it through the bypass, never here).
bool game_sv_Coop::DownBody(CEntity* entity, u16 who)
{
    game_sv_Coop* game = coop_server_game();
    if (!game) return false;
    CActor* body = BodyOf(entity);
    if (!body || !coop_client_of(body)) return false;
    xr_map<u16, SDowned>::iterator it = game->m_downed.find(body->ID());
    if (it == game->m_downed.end())
    {
        SDowned downed;
        downed.since = Device.dwTimeGlobal;
        downed.deadline = downed.since + game->m_bleedout_ms;
        downed.reviver = u16(-1);
        downed.revive_since = 0;
        downed.last_notice = 0;
        game->m_downed[body->ID()] = downed;
        CObject* killer = Level().Objects.net_Find(who);
        string256 text;
        xr_sprintf(text, "down|%u|%u|%s|%s", body->ID(), game->m_bleedout_ms / 1000, coop_body_player(body),
                   killer ? killer->cName().c_str() : "");
        SendLua(u16(-1), text);
        Msg("[COOP_SERVER] DOWN body=%u player=%s by=%u (%s) bleedout=%us", body->ID(), coop_body_player(body), who,
            killer ? killer->cName().c_str() : "?", game->m_bleedout_ms / 1000);
    }
    // The pin: whatever brought the health to zero (a hit, bleeding, radiation), the body keeps
    // the downed health and stays alive for the engine.
    body->SetfHealth(game->m_down_health);
    body->conditions().ClearWounds();
    return true;
}

// "Use" on a downed body by a living teammate (GE_COOP_USE_OBJECT): the revive starts; it
// completes in UpdateDowned if the reviver stays alive, up and within range.
void game_sv_Coop::ReviveStart(CActor* reviver, CActor* body)
{
    xr_map<u16, SDowned>::iterator it = m_downed.find(body->ID());
    if (it == m_downed.end() || !reviver || !reviver->g_Alive() || IsDowned(reviver->ID())) return;
    if (reviver->Position().distance_to(body->Position()) > m_revive_range) return;
    SDowned& downed = it->second;
    if (downed.reviver == reviver->ID()) return;
    downed.reviver = reviver->ID();
    downed.revive_since = Device.dwTimeGlobal;
    string256 text;
    xr_sprintf(text, "revive|%u|%u|%u|%s", body->ID(), reviver->ID(), m_revive_ms / 1000, coop_body_player(reviver));
    SendLua(u16(-1), text);
    Msg("[COOP_SERVER] REVIVE_START body=%u by=%u (%s)", body->ID(), reviver->ID(), coop_body_player(reviver));
}

void game_sv_Coop::Revive(u16 body_id, SDowned& downed, CActor* body)
{
    body->SetfHealth(m_revive_health);
    body->conditions().ClearWounds();
    string256 text;
    xr_sprintf(text, "revived|%u|%u", body_id, downed.reviver);
    SendLua(u16(-1), text);
    Msg("[COOP_SERVER] REVIVED body=%u by=%u after=%us", body_id, downed.reviver, (Device.dwTimeGlobal - downed.since) / 1000);
    m_downed.erase(body_id);
}

// The real death: the corpse keeps its inventory where it fell; the player gets a new, empty body.
void game_sv_Coop::KillDowned(u16 body_id, SDowned& downed, CActor* body, LPCSTR reason)
{
    xrClientData* client = coop_client_of(body);
    LPCSTR player = coop_body_player(body);
    Msg("[COOP_SERVER] DIED body=%u player=%s reason=%s downed=%us", body_id, player, reason, (Device.dwTimeGlobal - downed.since) / 1000);
    string256 text;
    xr_sprintf(text, "died|%u|%s|%s", body_id, player, reason);
    m_downed.erase(body_id);
    body->SetfHealth(0.f);
    body->KillEntity(body->ID(), TRUE); // bypasses DownBody
    SendLua(u16(-1), text);
    if (client) RespawnClient(client, body);
}

void game_sv_Coop::RespawnClient(xrClientData* client, CActor* corpse)
{
    TalkStop(client, false);
    TradeStop(client);
    // The corpse stays a plain server object; the client's assignment moves to the new body.
    client->owner = NULL;
    client->ps->GameID = u16(-1);
    // The standing is the player's, not the body's: the new body starts with the corpse's rank and reputation.
    CSE_ALifeCreatureActor* body = SpawnBody(client, false, corpse);
    Msg("[COOP_SERVER] RESPAWN client=%u player=%s corpse=%u body=%u rank=%d reputation=%d", client->ID.value(), client->name.c_str(),
        corpse->ID(), body ? body->ID : u16(-1), corpse->Rank(), corpse->Reputation());
    if (body)
    {
        string256 text;
        xr_sprintf(text, "respawn|%u|%u|%s", body->ID, corpse->ID(), client->name.c_str());
        SendLua(u16(-1), text);
    }
}

void game_sv_Coop::UpdateProfiles()
{
    if (m_profiles.empty()) return;
    xr_vector<u16> done;
    for (xr_map<u16, SBodyProfile>::iterator it = m_profiles.begin(); it != m_profiles.end(); ++it)
    {
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(it->first));
        if (!object) continue;
        ::luabind::functor<void> functor;
        if (ai().script_engine().functor("coop_server_actor.on_body_profile", functor))
        {
            CoopLuaActor coop_actor(object, false);
            functor(object->lua_game_object(), it->second.faction.c_str() ? it->second.faction.c_str() : "",
                    it->second.icon.c_str() ? it->second.icon.c_str() : "");
        }
        done.push_back(it->first);
    }
    for (u32 i = 0; i < done.size(); ++i) m_profiles.erase(done[i]);
}

void game_sv_Coop::UpdateItemStates()
{
    const u32 now = Device.dwTimeGlobal;
    if (now - m_item_states_checked < 1000) return;
    m_item_states_checked = now;
    xr_set<u16> seen;
    IClient* internal = server().GetServerClient();
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* data = static_cast<xrClientData*>(connection);
        if (!data->net_Accepted || !data->owner || !data->ps) return;
        CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(data->ps->GameID));
        if (!body || body->getDestroy()) return;
        TIItemContainer& items = body->inventory().m_all;
        for (TIItemContainer::iterator it = items.begin(); it != items.end(); ++it)
        {
            PIItem item = *it;
            if (!item || item->object().getDestroy()) continue;
            SItemState state;
            state.condition = item->GetCondition();
            CEatableItem* eatable = item->cast_eatable_item();
            state.uses = eatable ? eatable->GetRemainingUses() : 0xff;
            CWeapon* weapon = smart_cast<CWeapon*>(item);
            state.ammo = weapon ? u16(weapon->GetAmmoElapsed()) : 0xffff;
            // The place too: a returning player's items spawn on the client without the saved
            // state (CoopHideClientData), so the outfit, helmet, backpack and PDA fell into the
            // ruck by their default_to_ruck (132); the client follows the server's placement.
            state.place = item->m_ItemCurrPlace.value;
            const u16 id = item->object_id();
            seen.insert(id);
            xr_map<u16, SItemState>::iterator known = m_item_states.find(id);
            if (known != m_item_states.end() && fsimilar(known->second.condition, state.condition, 0.0005f) &&
                known->second.uses == state.uses && known->second.ammo == state.ammo && known->second.place == state.place)
                continue;
            m_item_states[id] = state;
            NET_Packet P;
            CGameObject::u_EventGen(P, GE_COOP_ITEM_STATE, id);
            P.w_float(state.condition);
            P.w_u8(state.uses);
            P.w_u16(state.ammo);
            P.w_u16(state.place);
            server().SendTo(data->ID, P, net_flags(TRUE, TRUE));
        }
    };
    server().ForEachClientDo(visit);
    // Items dropped, used up or parked away: forget them, so a returning one is sent afresh.
    for (xr_map<u16, SItemState>::iterator it = m_item_states.begin(); it != m_item_states.end();)
    {
        if (seen.find(it->first) == seen.end()) it = m_item_states.erase(it);
        else ++it;
    }
}

void game_sv_Coop::UpdateDowned()
{
    if (m_downed.empty()) return;
    const u32 now = Device.dwTimeGlobal;
    // Anyone still standing? With every connected player down, nobody can revive: all die now.
    bool someone_up = false;
    u32 connected = 0;
    IClient* internal = server().GetServerClient();
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* data = static_cast<xrClientData*>(connection);
        if (!data->net_Accepted || !data->owner || !data->ps) return;
        CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(data->ps->GameID));
        if (!body || !body->g_Alive()) return;
        ++connected;
        if (m_downed.find(body->ID()) == m_downed.end()) someone_up = true;
    };
    server().ForEachClientDo(visit);
    xr_vector<u16> ids;
    for (xr_map<u16, SDowned>::iterator it = m_downed.begin(); it != m_downed.end(); ++it) ids.push_back(it->first);
    for (u32 i = 0; i < ids.size(); ++i)
    {
        xr_map<u16, SDowned>::iterator it = m_downed.find(ids[i]);
        if (it == m_downed.end()) continue;
        SDowned& downed = it->second;
        CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(ids[i]));
        if (!body || !body->g_Alive() || !coop_client_of(body))
        {
            Msg("[COOP_SERVER] DOWN_DROPPED body=%u (gone, dead or unowned)", ids[i]);
            m_downed.erase(it);
            continue;
        }
        if (body->GetfHealth() < m_down_health) body->SetfHealth(m_down_health); // the pin
        if (!someone_up && connected)
        {
            KillDowned(ids[i], downed, body, "everyone down");
            continue;
        }
        if (now >= downed.deadline)
        {
            KillDowned(ids[i], downed, body, "bleedout");
            continue;
        }
        u32 progress = 0;
        if (downed.reviver != u16(-1))
        {
            CActor* reviver = smart_cast<CActor*>(Level().Objects.net_Find(downed.reviver));
            const bool close_enough = reviver && reviver->g_Alive() && !IsDowned(reviver->ID()) &&
                reviver->Position().distance_to(body->Position()) <= m_revive_range + 0.5f;
            if (!close_enough)
            {
                Msg("[COOP_SERVER] REVIVE_STOP body=%u by=%u", ids[i], downed.reviver);
                downed.reviver = u16(-1);
                string256 text;
                xr_sprintf(text, "revive|%u|65535|0|", ids[i]);
                SendLua(u16(-1), text);
            }
            else if (now - downed.revive_since >= m_revive_ms)
            {
                Revive(ids[i], downed, body);
                continue;
            }
            else
                progress = (now - downed.revive_since) * 100 / m_revive_ms;
        }
        if (now - downed.last_notice >= 1000)
        {
            downed.last_notice = now;
            string256 text;
            xr_sprintf(text, "downed|%u|%u|%u|%u", ids[i], (downed.deadline - now + 999) / 1000, downed.reviver, progress);
            SendLua(u16(-1), text);
        }
    }
}

static void coop_write_tip_text(NET_Packet& P, CGameObject* object, CUsableScriptObject* usable)
{
    CGameObject::u_EventGen(P, GE_COOP_TIP_TEXT, object->ID());
    LPCSTR text = usable->tip_text();
    P.w_u8(text ? 1 : 0);
    if (text) P.w_stringZ(text);
}

void game_sv_Coop::BroadcastTipText(CGameObject* object)
{
    if (!IsGameTypeCoop() || !OnServer() || !object || object->getDestroy()) return;
    CUsableScriptObject* usable = smart_cast<CUsableScriptObject*>(object);
    if (!usable) return;
    // Through the server's event queue: Process_event broadcasts GE_COOP_TIP_TEXT to all clients.
    NET_Packet P;
    coop_write_tip_text(P, object, usable);
    CGameObject::u_EventSend(P);
}

void game_sv_Coop::SendWorldInfos(xrClientData* client)
{
    CInventoryOwner* world = smart_cast<CInventoryOwner*>(Actor());
    if (!world || !client || !client->owner) return;
    xr_vector<shared_str> infos;
    world->coop_known_infos(infos);
    xr_string text;
    u32 chunks = 0;
    for (u32 i = 0; i <= infos.size(); ++i)
    {
        const bool flush = i == infos.size() || text.size() + infos[i].size() > 3800;
        if (flush && !text.empty())
        {
            SendLua(client->owner->ID, (xr_string("infos|") + text).c_str());
            text.clear();
            ++chunks;
        }
        if (i == infos.size()) break;
        if (!text.empty()) text += ",";
        text += infos[i].c_str();
    }
    Msg("[COOP_SERVER] WORLD_INFOS client=%u infos=%u chunks=%u", client->ID.value(), infos.size(), chunks);
}

void game_sv_Coop::SendTipTexts(xrClientData* client)
{
    u32 sent = 0;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(i));
        CUsableScriptObject* usable = object && !object->getDestroy() ? smart_cast<CUsableScriptObject*>(object) : NULL;
        if (!usable || !usable->tip_text() || smart_cast<CInventoryBox*>(object)) continue;
        NET_Packet P;
        coop_write_tip_text(P, object, usable);
        server().SendTo(client->ID, P, net_flags(TRUE, TRUE));
        ++sent;
    }
    Msg("[COOP_SERVER] TIP_TEXTS_SENT client=%u count=%u", client->ID.value(), sent);
}

// ---- shared PDA -------------------------------------------------------------------------------

static void coop_pda_send(NET_Packet& P, xrClientData* client)
{
    xrServer& server = *Level().Server;
    if (client)
    {
        server.SendTo(client->ID, P, net_flags(TRUE, TRUE));
        return;
    }
    IClient* internal = server.GetServerClient();
    xr_vector<ClientID> targets;
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* data = static_cast<xrClientData*>(connection);
        if (data->net_Accepted && data->owner) targets.push_back(data->ID);
    };
    server.ForEachClientDo(visit);
    for (u32 i = 0; i < targets.size(); ++i)
        server.SendTo(targets[i], P, net_flags(TRUE, TRUE));
}

// Where a map-spot object is in the world (ALife): the level name and position. Clients only
// know objects of their own level, so a spot on another level needs this to be drawn.
static void coop_pda_write_where(NET_Packet& P, u16 object_id)
{
    LPCSTR level_name = NULL;
    Fvector position;
    position.set(0.f, 0.f, 0.f);
    if (object_id != u16(-1) && ai().get_alife())
    {
        CSE_ALifeDynamicObject* se = ai().alife().objects().object(object_id, true);
        if (se && ai().game_graph().valid_vertex_id(se->m_tGraphID))
        {
            level_name = ai().game_graph().header().level(ai().game_graph().vertex(se->m_tGraphID)->level_id()).name().c_str();
            position = se->o_Position;
        }
    }
    P.w_u8(level_name ? 1 : 0);
    if (level_name)
    {
        P.w_stringZ(level_name);
        P.w_vec3(position);
    }
}

static void coop_pda_write_task(NET_Packet& P, CGameTask* task)
{
    P.w_begin(M_COOP_PDA);
    P.w_u8(1);
    P.w_stringZ(task->m_ID.c_str() ? task->m_ID.c_str() : "");
    P.w_u8(u8(task->GetTaskType()));
    P.w_u8(u8(task->GetTaskState()));
    P.w_u32(task->m_priority);
    P.w_stringZ(task->m_Title.c_str() ? task->m_Title.c_str() : "");
    P.w_stringZ(task->m_Description.c_str() ? task->m_Description.c_str() : "");
    P.w_stringZ(task->m_icon_texture_name.c_str() ? task->m_icon_texture_name.c_str() : "");
    P.w_stringZ(task->m_map_location.c_str() ? task->m_map_location.c_str() : "");
    P.w_u16(task->m_map_object_id);
    P.w_stringZ(task->m_map_hint.c_str() ? task->m_map_hint.c_str() : "");
    coop_pda_write_where(P, task->m_map_object_id);
}

void game_sv_Coop::OnTaskChanged(CGameTask* task)
{
    if (!coop_server_game() || !task) return;
    NET_Packet P;
    coop_pda_write_task(P, task);
    coop_pda_send(P, NULL);
    Msg("[COOP_SERVER] PDA_TASK id=%s state=%u map=%s/%u", task->m_ID.c_str(), u32(task->GetTaskState()),
        task->m_map_location.c_str() ? task->m_map_location.c_str() : "", task->m_map_object_id);
}

void game_sv_Coop::OnMapSpot(u8 op, LPCSTR spot, u16 id, LPCSTR hint, bool serializable)
{
    if (!coop_server_game()) return;
    NET_Packet P;
    P.w_begin(M_COOP_PDA);
    P.w_u8(op);
    if (op != 5) P.w_stringZ(spot ? spot : "");
    P.w_u16(id);
    if (op == 2 || op == 3) P.w_stringZ(hint ? hint : "");
    if (op == 2)
    {
        P.w_u8(serializable ? 1 : 0);
        coop_pda_write_where(P, id);
    }
    coop_pda_send(P, NULL);
}

// The client to address: the body Lua currently runs for, else everyone.
static xrClientData* coop_pda_context_client()
{
    CGameObject* body = game_sv_Coop::s_context_body;
    if (!body) return NULL;
    CSE_Abstract* entity = Level().Server->ID_to_entity(body->ID());
    return Level().Server->CoopControllerOf(entity);
}

void game_sv_Coop::OnGameNews(u8 type, LPCSTR caption, LPCSTR text, LPCSTR texture, int show_time)
{
    if (!coop_server_game()) return;
    NET_Packet P;
    P.w_begin(M_COOP_PDA);
    P.w_u8(6);
    P.w_u8(type);
    P.w_stringZ(caption ? caption : "");
    P.w_stringZ(text ? text : "");
    P.w_stringZ(texture ? texture : "");
    P.w_s32(show_time);
    coop_pda_send(P, coop_pda_context_client());
}

void game_sv_Coop::OnTalkMessage(LPCSTR caption, LPCSTR text, LPCSTR texture, LPCSTR templ)
{
    if (!coop_server_game()) return;
    xrClientData* client = coop_pda_context_client();
    if (!client) return; // a talk message only makes sense for the player who is talking
    NET_Packet P;
    P.w_begin(M_COOP_PDA);
    P.w_u8(7);
    P.w_stringZ(caption ? caption : "");
    P.w_stringZ(text ? text : "");
    P.w_stringZ(texture ? texture : "");
    P.w_stringZ(templ ? templ : "iconed_answer_item");
    coop_pda_send(P, client);
}

void game_sv_Coop::SendMapSpotTo(u16 body_id, bool add, LPCSTR spot, u16 object_id, LPCSTR hint)
{
    if (!coop_server_game() || !spot || !*spot) return;
    xrClientData* client = coop_client_of(smart_cast<CActor*>(Level().Objects.net_Find(body_id)));
    if (!client) return;
    NET_Packet P;
    P.w_begin(M_COOP_PDA);
    P.w_u8(add ? 2 : 4);
    P.w_stringZ(spot);
    P.w_u16(object_id);
    if (add)
    {
        P.w_stringZ(hint ? hint : "");
        P.w_u8(0); // not serializable: the server sends the player's markers again on every join
        coop_pda_write_where(P, object_id);
    }
    coop_pda_send(P, client);
}

bool game_sv_Coop::TravelTo(u16 body_id, u16 vertex, u32 level_vertex, const Fvector& position, const Fvector& angles)
{
    const GameGraph::_GRAPH_ID game_vertex = GameGraph::_GRAPH_ID(vertex);
    CActor* body = smart_cast<CActor*>(Level().Objects.net_Find(body_id));
    if (!body || !body->g_Alive() || !coop_server_game()) return false;
    if (!ai().game_graph().valid_vertex_id(game_vertex)) return false;
    NET_Packet change; // the M_CHANGE_LEVEL payload, as a changer's coop_request builds it
    change.w_begin(M_CHANGE_LEVEL);
    change.w(&game_vertex, sizeof(game_vertex));
    change.w(&level_vertex, sizeof(level_vertex));
    change.w_vec3(position);
    change.w_vec3(angles);
    Msg("[COOP_SERVER] TRAVEL_TO body=%u vertex=%u position=%f,%f,%f", body_id, u32(game_vertex), VPUSH(position));
    LevelChangeRequest(body, change);
    return true;
}

void game_sv_Coop::SendPda(xrClientData* client)
{
    u32 tasks = 0, spots = 0;
    vGameTasks& list = Level().GameTaskManager().GetGameTasks();
    for (u32 i = 0; i < list.size(); ++i)
    {
        if (!list[i].game_task) continue;
        NET_Packet P;
        coop_pda_write_task(P, list[i].game_task);
        coop_pda_send(P, client);
        ++tasks;
    }
    // Map spots set by the server Lua. Task-linked spots are recreated by the client's own task
    // copy (CGameTask::OnArrived), relation spots are per viewer: both skipped.
    Locations& locations = Level().MapManager().Locations();
    for (Locations_it it = locations.begin(); it != locations.end(); ++it)
    {
        CMapLocation* ml = it->location;
        if (!ml || ml->m_owner_task_id.size() || smart_cast<CRelationMapLocation*>(ml)) continue;
        // The actors' own markers (CActor::net_Spawn adds them for every actor, the world actor's
        // included): each client marks the actors it sees itself; the world actor is nobody's.
        if (!xr_strcmp(it->spot_type.c_str(), "actor_location") || !xr_strcmp(it->spot_type.c_str(), "actor_location_p")) continue;
        NET_Packet P;
        P.w_begin(M_COOP_PDA);
        P.w_u8(2);
        P.w_stringZ(it->spot_type.c_str() ? it->spot_type.c_str() : "");
        P.w_u16(it->object_id);
        LPCSTR hint = ml->GetHint();
        P.w_stringZ(hint ? hint : "");
        P.w_u8(ml->Serializable() ? 1 : 0);
        coop_pda_write_where(P, it->object_id);
        coop_pda_send(P, client);
        ++spots;
    }
    Msg("[COOP_SERVER] PDA_SENT client=%u tasks=%u spots=%u", client->ID.value(), tasks, spots);
}

// ---- Lua channel ------------------------------------------------------------------------------

// ---- Sounds for the clients (M_COOP_SOUND) --------------------------------------------------------
static const float coop_sound_range = 150.f;

// To the clients whose body is within earshot of the point (everyone when there is no point).
static void coop_sound_send(NET_Packet& P, const Fvector* position)
{
    xrServer& server = *Level().Server;
    IClient* internal = server.GetServerClient();
    xr_vector<ClientID> targets;
    auto visit = [&](IClient* connection)
    {
        if (connection == internal || !connection->flags.bConnected) return;
        xrClientData* data = static_cast<xrClientData*>(connection);
        if (!data->net_Accepted || !data->owner || !data->ps) return;
        if (position)
        {
            CObject* body = Level().Objects.net_Find(data->ps->GameID);
            if (!body || body->Position().distance_to(*position) > coop_sound_range) return;
        }
        targets.push_back(data->ID);
    };
    server.ForEachClientDo(visit);
    for (u32 i = 0; i < targets.size(); ++i) server.SendTo(targets[i], P, net_flags(TRUE, TRUE));
}

void game_sv_Coop::RelayScriptSound(u32 sid, LPCSTR path, u32 type, CObject* object, u8 mode, const Fvector* position,
                                    float delay, u32 flags, float volume, float frequency)
{
    if (!coop_server_game() || !path) return;
    // A sound bound to an actor is presentation of that player (the clients replay their own:
    // emissions, item use, the PDA voice of a phrase); the world's NPCs and objects are relayed.
    if (object && smart_cast<CActor*>(object)) return;
    Fvector where = position ? *position : (object ? object->Position() : Fvector().set(0, 0, 0));
    NET_Packet P;
    P.w_begin(M_COOP_SOUND);
    P.w_u8(1);
    P.w_u32(sid);
    P.w_stringZ(path);
    P.w_u32(type);
    P.w_u16(object ? object->ID() : u16(-1));
    P.w_u8(mode);
    P.w_vec3(where);
    P.w_float(delay);
    P.w_u32(flags);
    P.w_float(volume);
    P.w_float(frequency);
    coop_sound_send(P, (object || position) ? &where : NULL);
}

void game_sv_Coop::RelayScriptSoundStop(u32 sid, bool deferred)
{
    if (!coop_server_game()) return;
    NET_Packet P;
    P.w_begin(M_COOP_SOUND);
    P.w_u8(2);
    P.w_u32(sid);
    P.w_u8(deferred ? 1 : 0);
    coop_sound_send(P, NULL);
}

void game_sv_Coop::RelayScriptSoundPosition(u32 sid, const Fvector& position)
{
    if (!coop_server_game()) return;
    // A moving looped sound updates every frame; a few times a second is enough for the clients.
    static xr_map<u32, u32> last_sent;
    u32& last = last_sent[sid];
    if (Device.dwTimeGlobal - last < 250) return;
    last = Device.dwTimeGlobal;
    NET_Packet P;
    P.w_begin(M_COOP_SOUND);
    P.w_u8(3);
    P.w_u32(sid);
    P.w_vec3(position);
    coop_sound_send(P, &position);
}

void game_sv_Coop::RelayNpcSound(CObject* object, u32 internal_type, u32 index, u32 max_start, u32 min_start, u32 max_stop,
                                 u32 min_stop, LPCSTR prefix, u32 max_count, u32 type, u32 priority, u32 mask, LPCSTR bone)
{
    if (!coop_server_game() || !object) return;
    NET_Packet P;
    P.w_begin(M_COOP_SOUND);
    P.w_u8(4);
    P.w_u16(object->ID());
    P.w_u32(internal_type);
    P.w_u32(index);
    P.w_u32(max_start);
    P.w_u32(min_start);
    P.w_u32(max_stop);
    P.w_u32(min_stop);
    P.w_stringZ(prefix ? prefix : "");
    P.w_u32(max_count);
    P.w_u32(type);
    P.w_u32(priority);
    P.w_u32(mask);
    P.w_stringZ(bone ? bone : "");
    Fvector where = object->Position();
    coop_sound_send(P, &where);
}

void aim_target(shared_str const& aim_bone_id, Fvector& result, const CGameObject* object); // ai_stalker.cpp

void game_sv_Coop::RelayNpcShot(CObject* shooter, u16 weapon, const Fvector& position, const Fvector& direction)
{
    if (!coop_server_game() || !shooter) return;
    NET_Packet P;
    P.w_begin(M_COOP_SHOT);
    P.w_u16(weapon);
    P.w_vec3(position);
    P.w_vec3(direction); // full precision: a 16-bit direction is a metre off at thirty metres
    coop_sound_send(P, &position);
    // Probe (-coop_damage_probe): how far the shot's ray passes from a player body the stalker
    // is shooting at - the aim itself, before the weapon's dispersion.
    if (!strstr(Core.Params, "-coop_damage_probe")) return;
    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(shooter);
    const CEntityAlive* enemy = stalker ? stalker->memory().enemy().selected() : NULL;
    if (!enemy || !smart_cast<const CActor*>(enemy)) return;
    // the point the sight manager aims at: the stalker's aim bone when Lua set one, else the head
    // (CSightManager::aim_target for an actor)
    Fvector aim;
    ::aim_target(stalker->aim_bone_id().size() ? stalker->aim_bone_id() : shared_str("bip01_head"), aim, smart_cast<const CGameObject*>(enemy));
    Fvector to_aim = Fvector().sub(aim, position);
    const float along = to_aim.dotproduct(direction);
    const float miss = _sqrt(_max(0.f, to_aim.square_magnitude() - along * along));
    Msg("[COOP_NPC_AIM] npc=%u enemy=%u dist=%.1f miss=%.2f behind=%d visible=%d", shooter->ID(), enemy->ID(), along, miss,
        along < 0.f ? 1 : 0, stalker->memory().visual().visible_now(enemy) ? 1 : 0);
}

void game_sv_Coop::SendLua(u16 target, LPCSTR text)
{
    if (!coop_server_game() || !text) return;
    NET_Packet P;
    P.w_begin(M_COOP_LUA);
    P.w_stringZ(text);
    xrClientData* client = NULL;
    if (target != 0 && target != u16(-1))
    {
        CSE_Abstract* entity = Level().Server->ID_to_entity(target);
        client = entity ? Level().Server->CoopControllerOf(entity) : NULL;
        if (!client)
        {
            Msg("! [COOP_SERVER] LUA_SEND no client for body %u", target);
            return;
        }
    }
    coop_pda_send(P, client);
}

void game_sv_Coop::OnLuaMessage(xrClientData* client, NET_Packet& P)
{
    if (!client->owner || P.B.count < P.r_tell() + 1) return;
    shared_str text;
    P.r_stringZ(text);
    CGameObject* body = smart_cast<CGameObject*>(Level().Objects.net_Find(client->owner->ID));
    if (!body) return;
    ::luabind::functor<void> functor;
    if (!ai().script_engine().functor("coop_server_actor.on_client_lua", functor))
    {
        Msg("! [COOP_SERVER] LUA_MSG dropped: coop_server_actor.on_client_lua is not loaded");
        return;
    }
    CoopLuaActor coop_actor(body, false); // db.actor / AC_ID = the sender's body
    functor(body->lua_game_object(), text.c_str() ? text.c_str() : "");
}

// ---- trade with NPCs ---------------------------------------------------------------------------

void game_sv_Coop::SyncMoney(CInventoryOwner* owner)
{
    if (!coop_server_game() || !owner) return;
    CGameObject* object = smart_cast<CGameObject*>(owner);
    CActor* body = object ? game_sv_Coop::BodyOf(object) : NULL;
    if (!body) return;
    CSE_Abstract* entity = Level().Server->ID_to_entity(body->ID());
    xrClientData* client = entity ? Level().Server->CoopControllerOf(entity) : NULL;
    if (!client) return;
    NET_Packet P;
    CGameObject::u_EventGen(P, GE_MONEY, body->ID());
    P.w_u32(owner->get_money());
    Level().Server->SendTo(client->ID, P, net_flags(TRUE, TRUE));
}

void game_sv_Coop::OnTradeMessage(xrClientData* client, NET_Packet& P)
{
    if (P.B.count < P.r_tell() + 1) return;
    const u8 op = P.r_u8();
    switch (op)
    {
    case 1:
        if (P.B.count >= P.r_tell() + sizeof(u16)) TradeStart(client, P.r_u16());
        break;
    case 2:
    {
        if (P.B.count < P.r_tell() + 2) return;
        const bool buying = P.r_u8() != 0;
        const u8 count = P.r_u8();
        xr_vector<u16> ids;
        for (u8 i = 0; i < count && P.B.count >= P.r_tell() + sizeof(u16); ++i) ids.push_back(P.r_u16());
        TradeDeal(client, buying, ids);
        break;
    }
    case 3:
        TradeStop(client);
        break;
    default:
        break;
    }
}

static void coop_trade_refuse(xrClientData* client, u8 reason)
{
    NET_Packet P;
    P.w_begin(M_COOP_TRADE);
    P.w_u8(4);
    P.w_u8(reason);
    Level().Server->SendTo(client->ID, P, net_flags(TRUE, TRUE));
}

// The Lua the actor menu runs in SP when it enters or leaves trade mode (CUIActorMenu::CurModeToScript
// -> actor_menu.actor_menu_mode 2/0 -> trade_wnd_opened/closed): ActorMenu_on_trade_started/closed
// listeners (mob_trade counts the money of the deals for the NPC's logic, item_device hides the
// device charge from the prices) and the "trade_wnd_open" info. In coop the menu is the client's, so
// the server runs the same for the body; the window functions are called directly because
// actor_menu_mode keeps one last_mode for the whole VM, which two trading players would share.
static void coop_trade_lua(CActor* body, LPCSTR function)
{
    CoopLuaActor context(body, false);
    ::luabind::functor<void> functor;
    if (ai().script_engine().functor(function, functor))
        functor();
    else
        Msg("! [COOP_SERVER] TRADE_LUA missing %s", function);
}

void game_sv_Coop::TradeStart(xrClientData* client, u16 npc_id)
{
    TradeStop(client);
    CActor* body = coop_talk_body(client);
    CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(npc_id));
    xr_map<u32, SCoopTalk>::iterator talk = m_talks.find(client->ID.value());
    if (!body || !npc || talk == m_talks.end() || talk->second.npc != npc_id)
    {
        coop_trade_refuse(client, 1); // not in a conversation with this NPC
        return;
    }
    // No IsTradeEnabled here: Anomaly's traders and mechanics carry trade_enable = false in their
    // [meet] (that hides the engine's talk-window button); the dialog line npc:start_trade is the
    // authority, as in SP where CScriptGameObject::StartTrade opens the window regardless.
    CTrade* body_trade = body->GetTrade();
    CTrade* npc_trade = npc->GetTrade();
    if (!body_trade || !npc_trade)
    {
        coop_trade_refuse(client, 3);
        return;
    }
    // As CUIActorMenu::InitTradeMode does in SP, on the server side only.
    npc->StartTrading();
    body->StartTrading();
    body_trade->StartTradeEx(npc);
    npc_trade->StartTradeEx(body);
    SCoopTrade& trade = m_trades[client->ID.value()];
    trade.npc = npc_id;
    coop_talk_speaker(npc_id);
    coop_trade_lua(body, "actor_menu.trade_wnd_opened");
    Msg("[COOP_SERVER] TRADE_START client=%u body=%u npc=%u", client->ID.value(), body->ID(), npc_id);
    TradeSendPrices(client, 1);
}

void game_sv_Coop::TradeStartFor(CActor* body, u16 npc_id)
{
    game_sv_Coop* game = coop_server_game();
    if (!game || !body) return;
    CSE_Abstract* entity = Level().Server->ID_to_entity(body->ID());
    xrClientData* client = entity ? Level().Server->CoopControllerOf(entity) : NULL;
    if (client) game->TradeStart(client, npc_id);
}

void game_sv_Coop::UpgradeStartFor(CActor* body, u16 npc_id)
{
    game_sv_Coop* game = coop_server_game();
    if (!game || !body) return;
    CSE_Abstract* entity = Level().Server->ID_to_entity(body->ID());
    xrClientData* client = entity ? Level().Server->CoopControllerOf(entity) : NULL;
    if (!client) return;
    string64 text;
    xr_sprintf(text, "upgrade|%u", u32(npc_id));
    SendLua(body->ID(), text);
    Msg("[COOP_SERVER] UPGRADE_START client=%u body=%u npc=%u", client->ID.value(), body->ID(), npc_id);
}

void game_sv_Coop::TradeSendPrices(xrClientData* client, u8 op)
{
    xr_map<u32, SCoopTrade>::iterator it = m_trades.find(client->ID.value());
    CActor* body = coop_talk_body(client);
    if (it == m_trades.end() || !body) return;
    CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(it->second.npc));
    if (!npc) return;
    CTrade* npc_trade = npc->GetTrade();
    NET_Packet P;
    P.w_begin(M_COOP_TRADE);
    P.w_u8(op);
    P.w_u16(it->second.npc);
    P.w_u32(npc->get_money()); // the NPC's replica on the client only has the money of its spawn packet
    xr_vector<std::pair<u16, u32>> prices;
    // The NPC's CTrade prices both ways: b_buying = the NPC buys (the actor sells).
    for (TIItemContainer::const_iterator i = body->inventory().m_all.begin(); i != body->inventory().m_all.end(); ++i)
    {
        const u32 price = npc_trade->GetItemPrice(*i, true);
        if (price) prices.push_back(std::make_pair((*i)->object().ID(), price));
    }
    for (TIItemContainer::const_iterator i = npc->inventory().m_all.begin(); i != npc->inventory().m_all.end(); ++i)
    {
        const u32 price = npc_trade->GetItemPrice(*i, false);
        if (price) prices.push_back(std::make_pair((*i)->object().ID(), price));
    }
    if (prices.size() > 60000) prices.resize(60000);
    P.w_u16(u16(prices.size()));
    for (u32 i = 0; i < prices.size(); ++i)
    {
        P.w_u16(prices[i].first);
        P.w_u32(prices[i].second);
    }
    server().SendTo(client->ID, P, net_flags(TRUE, TRUE));
}

void game_sv_Coop::TradeDeal(xrClientData* client, bool buying, const xr_vector<u16>& ids)
{
    xr_map<u32, SCoopTrade>::iterator it = m_trades.find(client->ID.value());
    CActor* body = coop_talk_body(client);
    if (it == m_trades.end() || !body || ids.empty()) return;
    CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(it->second.npc));
    if (!npc) return;
    CTrade* npc_trade = npc->GetTrade();
    // Items must sit with the side that sells them; the NPC's CTrade prices them (b_buying = NPC buys).
    xr_vector<PIItem> items;
    u32 total = 0;
    for (u32 i = 0; i < ids.size(); ++i)
    {
        PIItem item = buying ? npc->inventory().GetItemFromInventory(ids[i]) : body->inventory().GetItemFromInventory(ids[i]);
        if (!item || item->object().getDestroy()) continue;
        const u32 price = npc_trade->GetItemPrice(item, !buying);
        if (!price) continue;
        items.push_back(item);
        total += price;
    }
    if (items.empty()) return;
    if (buying && body->get_money() < total)
    {
        coop_trade_refuse(client, 5); // not enough money
        return;
    }
    if (!buying && npc->get_money() < total)
    {
        coop_trade_refuse(client, 6);
        return;
    }
    // CUIActorMenu::OnBtnPerformTradeBuy/Sell: callback, then the transfers (events + money).
    // The NPC's Lua callbacks (mob_trade counting the deal, trade_sell_buy_item) see the body as the actor.
    {
        CoopLuaActor context(body, false);
        npc_trade->OnPerformTrade(buying ? total : 0, buying ? 0 : total);
        for (u32 i = 0; i < items.size(); ++i)
            npc_trade->TransferItem(items[i], !buying);
    }
    npc->set_money(npc->get_money(), true);
    body->set_money(body->get_money(), true);
    Msg("[COOP_SERVER] TRADE_DEAL client=%u body=%u npc=%u buying=%u items=%u total=%u money=%u", client->ID.value(), body->ID(),
        it->second.npc, buying ? 1 : 0, u32(items.size()), total, body->get_money());
    TradeSendPrices(client, 2);
}

void game_sv_Coop::TradeStop(xrClientData* client)
{
    xr_map<u32, SCoopTrade>::iterator it = m_trades.find(client->ID.value());
    if (it == m_trades.end()) return;
    CActor* body = client->ps ? smart_cast<CActor*>(Level().Objects.net_Find(client->ps->GameID)) : NULL;
    CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(it->second.npc));
    if (body)
    {
        // Before the sides part: mob_trade finds the NPC by its still-talking state.
        coop_trade_lua(body, "actor_menu.trade_wnd_closed");
        if (body->GetTrade()) body->GetTrade()->StopTrade();
        body->StopTrading();
    }
    if (npc)
    {
        if (npc->GetTrade()) npc->GetTrade()->StopTrade();
        npc->StopTrading();
    }
    Msg("[COOP_SERVER] TRADE_STOP client=%u npc=%u", client->ID.value(), it->second.npc);
    m_trades.erase(it);
}

void game_sv_Coop::PrepareClient(xrClientData* client)
{
    if (client == server().GetServerClient()) return;
    R_ASSERT(m_bootstrap_reported && strstr(Core.Params, "-coop_server_network_test"));
    R_ASSERT(client->ps);
    if (client->owner) return; // Repeated connection-data request must not duplicate bodies.
    if (ReclaimBody(client)) return;
    SpawnBody(client, true);
}

// A new body for the player at the level's entry (Anomaly's new-game start on Cordon, the world
// actor's place elsewhere): on a first connection with the [loadout], after a death without it.
CSE_ALifeCreatureActor* game_sv_Coop::SpawnBody(xrClientData* client, bool with_loadout, const CInventoryOwner* standing_from)
{
    CSE_ALifeCreatureActor* world = alife().graph().actor();
    Fvector position = world->o_Position;
    u32 seed_node = world->m_tNodeID;
    // Use Anomaly's own new-game start. The world identity stays in place.
    if (!strstr(Core.Params, "-coop_spatial_probe") &&
        !xr_strcmp(alife().level_name().c_str(), "l01_escape"))
    {
        string_path locations_path;
        FS.update_path(locations_path, "$game_config$", "plugins\\new_game_start_locations.ltx");
        CInifile locations(locations_path);
        position.set(locations.r_float("rookie_village", "x"),
            locations.r_float("rookie_village", "y"), locations.r_float("rookie_village", "z"));
        seed_node = locations.r_u32("rookie_village", "lvid");
        R_ASSERT2(ai().level_graph().valid_vertex_id(seed_node), "COOP invalid rookie village AI vertex");
    }
    position.x += 2.f + float(client->ID.value() % 3);
    u32 node = ai().level_graph().vertex(seed_node, position);
    R_ASSERT(ai().level_graph().valid_vertex_id(node));
    if (strstr(Core.Params, "-coop_spatial_probe") && client->ID.value() == 3)
    {
        bool found = false;
        for (u32 candidate = 0; candidate < ai().level_graph().header().vertex_count(); ++candidate)
        {
            const Fvector point = ai().level_graph().vertex_position(candidate);
            const float distance = world->o_Position.distance_to(point);
            if (distance > alife().offline_distance() + 100.f && distance < alife().offline_distance() + 150.f)
            {
                node = candidate;
                found = true;
                break;
            }
        }
        R_ASSERT2(found, "COOP_SPATIAL no distant test spawn on this level");
    }
    position = ai().level_graph().vertex_position(node);
    const auto graph_vertex = ai().cross_table().vertex(node).game_vertex_id();
    CSE_ALifeCreatureActor* body = smart_cast<CSE_ALifeCreatureActor*>(
        alife().spawn_item("actor", position, node, graph_vertex, u16(-1), true));
    R_ASSERT(body && !body->s_flags.is(M_SPAWN_OBJECT_ASPLAYER));
    Msg("[COOP_TRACE] CSE_CREATED id=%u health=%f online=%d position=%f,%f,%f", body->ID, body->get_health(), body->m_bOnline, VPUSH(body->o_Position));
    body->CSE_ALifeObject::can_switch_offline(false);
    COOP_LOADOUT loadout;
    u32 money = 0;
    // The character from the join menu: its faction and icon always, its loadout and money on a
    // first body; the server's [loadout] when the client came without one (Play-*.cmd).
    shared_str faction, icon;
    u32 profile_money = 0;
    COOP_LOADOUT profile_loadout;
    const bool profile_loadout_given = coop_parse_profile(client->coop_profile.c_str(), faction, icon, profile_money, profile_loadout);
    if (with_loadout)
    {
        if (profile_loadout_given)
        {
            loadout = profile_loadout;
            money = profile_money;
        }
        else
            coop_read_loadout(loadout, money);
    }
    body->m_dwMoney = money; // read by CActor::net_Spawn from the spawn packet written on going online
    if (faction.size())
    {
        // The community rides on the spawn packet: the clients' replicas (the player's own included)
        // show it and colour NPC relations by it; the live server body gets its team from it too
        // (on_body_profile below, set_character_community -> ChangeTeam).
        string64 community_id;
        xr_sprintf(community_id, "%s%s", strncmp(faction.c_str(), "actor_", 6) ? "actor_" : "", faction.c_str());
        const CHARACTER_COMMUNITY_INDEX index = CHARACTER_COMMUNITY::IdToIndex(shared_str(community_id), NO_COMMUNITY_INDEX, true);
        if (index != NO_COMMUNITY_INDEX) body->m_community_index = index;
        else Msg("! [COOP_SERVER] BODY_PROFILE unknown community %s", community_id);
    }
    // The body is the player's: its character name is the connection name (shown in game and the
    // key a saved world is matched back to the player by, see Create), its object name readable.
    {
        // The join menu keeps the name's spaces as '_' (the SP way); the character shows them as spaces.
        string256 shown;
        xr_strcpy(shown, client->name.c_str());
        for (char* c = shown; *c; ++c) if (*c == '_') *c = ' ';
        body->m_character_name_str = shown;
        body->m_character_name = shown;
    }
    {
        string256 object_name;
        xr_sprintf(object_name, "coop_%s_%u", client->name.c_str(), body->ID);
        for (char* c = object_name; *c; ++c)
        {
            const bool ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_';
            if (!ok) *c = '_';
        }
        body->set_name_replace(object_name);
    }
    // The client's before the spawn goes out: a connected client (respawn after a death) gets the
    // body's spawn as its own from Process_spawn (WriteOwnedConnectSpawn), the others as a replica.
    client->owner = body;
    client->ps->GameID = body->ID;
    if (standing_from) // before the spawn packet is written: the live body reads them from it
    {
        body->m_rank = standing_from->Rank();
        body->m_reputation = standing_from->Reputation();
    }
    if (!body->m_bOnline) alife().switch_online(body);
    Msg("[COOP_TRACE] CSE_ONLINE id=%u health=%f position=%f,%f,%f", body->ID, body->get_health(), VPUSH(body->o_Position));
    R_ASSERT(body->owner == server().GetServerClient());
    if (with_loadout)
    {
        coop_spawn_loadout(alife(), body, loadout);
        Msg("[COOP_SERVER] LOADOUT_MONEY body=%u money=%u source=%s", body->ID, money, profile_loadout_given ? "menu" : "server");
    }
    if (faction.size() || icon.size())
    {
        // Community and PDA icon go on the live body once the internal client has spawned it
        // (UpdateProfiles), through the server Lua.
        SBodyProfile& profile = m_profiles[body->ID];
        profile.faction = faction;
        profile.icon = icon;
        Msg("[COOP_SERVER] BODY_PROFILE body=%u faction=%s icon=%s", body->ID, faction.c_str() ? faction.c_str() : "", icon.c_str() ? icon.c_str() : "");
    }
    R_ASSERT(alife().graph().actor() == world && Actor()->ID() == world->ID);
    Msg("[COOP_SERVER] PLAYER_BODY client=%u body=%u world=%u loadout=%u", client->ID.value(), body->ID, world->ID, with_loadout ? 1 : 0);
    return body;
}

void game_sv_Coop::ReleaseClient(xrClientData* client)
{
    if (client == server().GetServerClient()) return;
    TalkStop(client, false);
    CSE_Abstract* assigned = client->owner;
    const u16 id = assigned && client->ps ? client->ps->GameID : u16(-1);
    client->owner = NULL;
    CSE_Abstract* record = server().ID_to_entity(id);
    if (assigned && record == assigned && smart_cast<CSE_ALifeCreatureActor*>(record) &&
        id != alife().graph().actor()->ID)
    {
        // A living body waits for its player; a dead one is removed as before.
        if (!ParkBody(client, smart_cast<CSE_ALifeCreatureActor*>(record)))
        {
            CGameObject* body = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
            if (body && !body->getDestroy()) body->DestroyObject();
        }
    }
    Msg("[COOP_SERVER] PLAYER_LEAVE client=%u body=%u world=%u", client->ID.value(), id, alife().graph().actor()->ID);
}

void game_sv_Coop::OnPlayerConnectFinished(ClientID id)
{
    inherited::OnPlayerConnectFinished(id);
    if (m_bootstrap_reported)
    {
        xrClientData* client = server().ID_to_client(id);
        if (client && client != server().GetServerClient())
        {
            R_ASSERT(client->owner && client->owner->owner == server().GetServerClient());
            Msg("[COOP_SERVER] PLAYER_READY client=%u body=%u", id.value(), client->owner->ID);
            SendTipTexts(client);
            SendPda(client);
            SendWorldInfos(client);
            // The server Lua hands this player's client what the world already decided (weather...).
            CGameObject* body = smart_cast<CGameObject*>(Level().Objects.net_Find(client->owner->ID));
            ::luabind::functor<void> functor;
            if (body && ai().script_engine().functor("coop_server_actor.on_client_ready", functor))
            {
                CoopLuaActor coop_actor(body, false);
                functor(body->lua_game_object());
            }
        }
        return;
    }
    IClient* internal = server().GetServerClient();
    CSE_ALifeCreatureActor* actor = alife().graph().actor();
    R_ASSERT2(internal && internal->ID == id, "COOP_BOOTSTRAP ready from non-internal client");
    R_ASSERT2(actor && actor->owner == internal, "COOP_BOOTSTRAP world actor owner mismatch");
    u32 unowned = 0;
    for (u32 i = 0; i < server().GetEntitiesNum(); ++i)
    {
        CSE_Abstract* object = server().GetEntity(i);
        if (!object->owner && !object->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
            ++unowned;
    }
    R_ASSERT2(unowned == 0, "COOP_BOOTSTRAP unowned online entities after internal join");
    m_bootstrap_reported = true;
    m_probe_started = m_probe_last_report = Device.dwTimeGlobal;
    if (m_server_probe)
        Msg("[COOP_SERVER] INTERNAL_OWNER_READY actor=%u client=%u", actor->ID, id.value());
    Msg("[COOP_BOOTSTRAP] READY internal_client=%u world_actor=%u online_entities=%u unowned=%u",
        id.value(), actor->ID, server().GetEntitiesNum(), unowned);
}

game_sv_Coop::~game_sv_Coop()
{
    if (m_server_probe)
        Msg("[COOP_SERVER] WORLD_RELEASE_BEGIN ticks=%u timed_stop=%u", m_probe_ticks, m_probe_stopping ? 1 : 0);
}

void game_sv_Coop::Update()
{
    inherited::Update();
    TalkUpdate();
    if (!m_server_probe || !m_bootstrap_reported || m_probe_stopping)
        return;
    ++m_probe_ticks;
    UpdateDowned();
    UpdateProfiles();
    UpdateItemStates();
    u32 now = Device.dwTimeGlobal;
    // Autosave ([server] autosave_minutes, 0 = off) while anyone plays: the server's own "save"
    // console command, the same path as a player's.
    if (m_autosave_ms && now - m_last_autosave >= m_autosave_ms)
    {
        m_last_autosave = now;
        if (!m_parked.empty() || server().GetClientsCount() > 1)
        {
            Msg("[COOP_SERVER] AUTOSAVE");
            Console->Execute("save coop_autosave");
        }
    }
    if (strstr(Core.Params, "-coop_server_actor_roles"))
    {
        CSE_ALifeCreatureActor* world = alife().graph().actor();
        R_ASSERT(world && world->ID == 0 && Actor() && Actor()->ID() == world->ID);
        const u32 elapsed = now - m_probe_started;
        if (!m_probe_body_created && elapsed >= 10000)
        {
            Fvector position = world->o_Position;
            position.x += 2.f;
            u32 node = ai().level_graph().vertex(world->m_tNodeID, position);
            R_ASSERT(ai().level_graph().valid_vertex_id(node));
            position = ai().level_graph().vertex_position(node);
            CSE_Abstract* object = alife().spawn_item("actor", position, node, world->m_tGraphID, u16(-1), true);
            CSE_ALifeCreatureActor* body = smart_cast<CSE_ALifeCreatureActor*>(object);
            R_ASSERT(body && body->ID != world->ID);
            R_ASSERT(!body->s_flags.is(M_SPAWN_OBJECT_ASPLAYER));
            body->m_bALifeControl = true;
            body->CSE_ALifeObject::can_switch_offline(false);
            // spawn_item registered the body before supplies/on_spawn callbacks.
            m_probe_body = body->ID;
            m_probe_body_created = true;
            R_ASSERT(alife().graph().actor() == world);
            Msg("[COOP_SERVER] BODY_CREATED id=%u world=%u asplayer=0", body->ID, world->ID);
        }
        if (m_probe_body_created && !m_probe_routing_tested && elapsed >= 15000)
        {
            CSE_Abstract* body = server().ID_to_entity(m_probe_body);
            R_ASSERT(body && Level().Objects.net_Find(m_probe_body));
            server().TestCoopSpawnRouting(body, world);
            m_probe_routing_tested = true;
        }
        if (m_probe_body_created && !m_probe_body_released && elapsed >= 40000)
        {
            CSE_ALifeDynamicObject* body = smart_cast<CSE_ALifeDynamicObject*>(server().ID_to_entity(m_probe_body));
            R_ASSERT(body && body->m_bOnline && body->owner == server().GetServerClient());
            R_ASSERT(Level().Objects.net_Find(m_probe_body));
            Msg("[COOP_SERVER] BODY_RELEASE id=%u world=%u", m_probe_body, world->ID);
            CGameObject* client_body = smart_cast<CGameObject*>(Level().Objects.net_Find(m_probe_body));
            R_ASSERT(client_body && client_body->Local());
            client_body->DestroyObject(); // Online removal must notify replicas before ALife release.
            m_probe_body_released = true;
        }
    }
    if (now - m_probe_last_report >= 5000)
    {
        m_probe_last_report = now;
        IClient* internal = server().GetServerClient();
        CSE_ALifeCreatureActor* actor = alife().graph().actor();
        R_ASSERT2(internal && actor && actor->owner == internal, "COOP_SERVER lost world owner");
        u32 unowned = 0;
        for (u32 i = 0; i < server().GetEntitiesNum(); ++i)
        {
            CSE_Abstract* object = server().GetEntity(i);
            if (!object->owner && !object->s_flags.is(M_SPAWN_OBJECT_PHANTOM)) ++unowned;
        }
        Msg("[COOP_SERVER] TICK elapsed_ms=%u ticks=%u game_time=%llu entities=%u unowned=%u",
            now - m_probe_started, m_probe_ticks, GetGameTime(), server().GetEntitiesNum(), unowned);
        auto report_anchor = [&](IClient* connection)
        {
            if (connection == internal || !connection->flags.bConnected) return;
            xrClientData* client = static_cast<xrClientData*>(connection);
            if (!client->owner || !client->ps) return;
            CSE_ALifeCreatureActor* body = smart_cast<CSE_ALifeCreatureActor*>(client->owner);
            if (!body) return;
            Msg("[COOP_SPATIAL] ANCHOR client=%u body=%u position=%f,%f,%f", client->ID.value(), body->ID, VPUSH(body->o_Position));
        };
        // Do not call activation_distance inside this iteration: its client lock is non-recursive.
        server().ForEachClientDo(report_anchor);
        if (CoopConsoleEnabled())
        {
            CServerInfo info;
            string128 value;
            info.AddItem("Mode", "Coop server probe");
            info.AddItem("Map", alife().level_name().c_str());
            if (strstr(Core.Params, "-coop_server_network_test"))
            {
                xr_sprintf(value, "%u / 2", server().GetClientsCount() > 0 ? server().GetClientsCount() - 1 : 0);
                info.AddItem("External connections", value);
            }
            else info.AddItem("External players", "0 (admission disabled)");
            info.AddItem("World owner", "Internal connection");
            xr_sprintf(value, "%u", server().GetPort()); info.AddItem("Port", value);
            xr_sprintf(value, "%u", server().GetEntitiesNum()); info.AddItem("Server objects", value);
            xr_sprintf(value, "%u", m_probe_ticks); info.AddItem("Simulation ticks", value);
            xr_sprintf(value, "%u", unowned); info.AddItem("Unowned objects", value);
            xr_sprintf(value, "%llu", GetGameTime()); info.AddItem("Game time (ms)", value);
            info.AddItem("Stop", "quit / window close");
            CoopConsoleSetInfo(info);
        }
        R_ASSERT2(!unowned, "COOP_SERVER found unowned online entity");
        if (strstr(Core.Params, "-coop_server_network_test")) FlushLog();
    }
    if (m_probe_duration && now - m_probe_started >= m_probe_duration)
    {
        if (strstr(Core.Params, "-coop_server_actor_roles"))
        {
            R_ASSERT(m_probe_body_created && m_probe_body_released && m_probe_routing_tested);
            R_ASSERT(!Level().Objects.net_Find(m_probe_body) && !server().ID_to_entity(m_probe_body));
            R_ASSERT(alife().graph().actor()->ID == 0 && Actor() && Actor()->ID() == 0);
            Msg("[COOP_SERVER] ACTOR_ROLES_PASS world=0 body_removed=1");
        }
        m_probe_stopping = true;
        Msg("[COOP_SERVER] TIMED_STOP ticks=%u", m_probe_ticks);
        Console->Execute("quit");
    }
}
