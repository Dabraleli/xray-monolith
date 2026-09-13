#pragma once
#include "coop_player_store.h"
#include "game_sv_single.h"
#include "PhraseDialogDefs.h"

class CSE_ALifeCreatureActor;
class CGameObject;
class CActor;
class CEntity;
class CGameTask;
class CInventoryOwner;

// Anomaly's Lua addresses "the player" as db.actor, which on the coop server is the world actor:
// a hidden identity with no body near anyone. While Lua runs on behalf of a player body (its
// dialog, an NPC's schemes next to it, a hit or a kill by it) db.actor and the global AC_ID
// (the "actor id" constant, 0 in _g.script) are swapped to that body and restored afterwards.
// dialog_scope marks the body's own dialog scripts (see InDialogScope).
struct CoopLuaActor
{
    luabind::object db;
    luabind::object saved;
    luabind::object globals;
    luabind::object saved_ac_id;
    CGameObject* saved_context;
    bool active;
    bool dialog_scope;
    CoopLuaActor(CGameObject* body, bool dialog_scope = true);
    ~CoopLuaActor();
};

// Bootstrap milestone: one server-side world and its internal client.
// External player admission is deliberately unavailable until actor ownership
// and client binders are implemented. This is not the final dedicated runtime.
class game_sv_Coop : public game_sv_Single
{
    typedef game_sv_Single inherited;
    bool m_bootstrap_reported;
    bool m_server_probe;
    bool m_probe_stopping;
    u32 m_probe_started;
    u32 m_probe_last_report;
    u32 m_probe_duration;
    u32 m_probe_ticks;
    u16 m_probe_body;
    bool m_probe_body_created;
    bool m_probe_body_released;
    bool m_probe_routing_tested;
    // Player identity is the connection name (name=... in the client's -start options). A body
    // whose player left waits offline in ALife with its inventory and saved actor state; the
    // same name gets it back on reconnect, another name gets a new body.
    struct SParkedBody
    {
        u16 id;
        xr_vector<u8> state; // CActor::net_Save payload, applied through CSE client_data on spawn
        xr_map<u16, xr_vector<u8>> items; // the same for the inventory items (remaining uses, condition)
        u32 parked_at;
    };
    xr_map<shared_str, SParkedBody> m_parked;
    bool ReclaimBody(xrClientData* client);
    bool ParkBody(xrClientData* client, CSE_ALifeCreatureActor* body);
    // Per-player Lua state the client's presentation modules keep (thirst, sleep): the client
    // sends it as a text blob (M_COOP_PLAYER_STORE), the server keeps it under the connection
    // name and hands it back when the same name is ready in the world again.
    xr_map<shared_str, xr_string> m_store;
    xr_map<shared_str, CCoopStoreAssembler> m_store_parts; // the upload in flight, per connection name
    // NPC dialogs (M_COOP_TALK). The dialog runs on the server between the player's body and the
    // NPC exactly as CUITalkWnd drives it in SP; the client only renders what it is sent. While the
    // dialog scripts run, Lua db.actor points at the body, so rewards and checks address the player.
    struct SCoopTalk
    {
        u16 npc;
        DIALOG_SHARED_PTR dialog; // NULL = topic selection
    };
    xr_map<u32, SCoopTalk> m_talks; // by client id
public:
    void TalkStart(xrClientData* client, u16 npc_id); // also from CActor::RunTalkDialog for scripted starts
private:
    void TalkSelect(xrClientData* client, const shared_str& id);
    void TalkStop(xrClientData* client, bool notify);
    void TalkSay(xrClientData* client, SCoopTalk& talk, const shared_str& phrase_id);
    void TalkSendQuestions(xrClientData* client, SCoopTalk& talk);
    void TalkAnswer(xrClientData* client, bool ours, LPCSTR text);
    void TalkUpdate();
protected:
    virtual void OnEvent(NET_Packet& packet, u16 type, u32 time, ClientID sender);
public:
    void OnPlayerStore(xrClientData* client, NET_Packet& P);
    void OnTalkMessage(xrClientData* client, NET_Packet& P);
    // From CAI_PhraseDialogManager::AnswerPhrase on the headless server: the NPC's reply to a body.
    static void TalkNpcAnswer(CGameObject* listener, LPCSTR text);
    // True while a body's dialog scripts run (db.actor swapped to the body): stop_talk from there
    // is the dialog's own decision, stop_talk from NPC schemes outside it is ignored.
    static u32 s_dialog_scope;
    static bool InDialogScope() { return s_dialog_scope > 0; }
    // The body Lua currently runs on behalf of (set by CoopLuaActor), NULL outside such a scope.
    // Engine bindings that resolve "the actor" by id (level.object_by_id(0) behind
    // get_story_object("actor")) hand out this body instead of the world actor.
    static CGameObject* s_context_body;
    static CGameObject* ContextActor(u16 requested_id);
    // "Where is the actor?" for the world actor's Lua-visible position (se_actor.position,
    // db.actor:position()) on the coop server: the body Lua runs for; else the living body nearest
    // to the ALife object being updated or switched (CoopSpatialScope, set around the scheduled
    // update and the switch checks); else the body nearest to the world actor's own position.
    // False (the real position) on clients, in other modes and with no living body.
    static const CSE_Abstract* s_spatial_context;
    static bool WorldActorAnchor(const CSE_Abstract* entity, Fvector& position);
    static bool WorldActorAnchor(const CObject* object, Fvector& position);
    // The living player body nearest to a point (NULL on clients, in other modes, or with no bodies):
    // what an NPC treats as "the actor" for looks, schemes and the player-on-the-path logic.
    static CActor* NearestBody(const Fvector& position);
    // An NPC whose Lua runs for one player (companions): its binder updates use the owner's body
    // as db.actor while that player is in the world (level.coop_set_lua_owner from the server Lua).
    static void SetLuaOwner(u16 object_id, LPCSTR player);
    static CActor* BodyOfPlayer(LPCSTR player); // the living body of a connected player, or NULL
    static LPCSTR PlayerOfBody(u16 body_id); // the connection name, "" when not a player's body
    static CActor* ContextBodyFor(CGameObject* object); // owner's body, else the nearest one
    // The object as a player body on the coop server (NULL for NPCs, the world actor, other modes).
    static CActor* BodyOf(const CObject* object);
    // Hint of a script-usable object (door "open"/"close"...) set by the server Lua: broadcast to
    // every client (GE_COOP_TIP_TEXT), or all current hints to one client that has just connected.
    static void BroadcastTipText(CGameObject* object);
    void SendTipTexts(xrClientData* client);
    // The shared PDA (M_COOP_PDA): one task list and one map for everyone. The server's
    // CGameTaskManager and CMapManager are the truth; every change is mirrored to all clients,
    // and a client that has just connected gets the whole current state. Game news go to the
    // client whose body the Lua runs for (CoopLuaActor scope) or to everyone outside such a scope.
    static void OnTaskChanged(CGameTask* task);
    static void OnMapSpot(u8 op, LPCSTR spot, u16 id, LPCSTR hint, bool serializable);
    static void OnGameNews(u8 type, LPCSTR caption, LPCSTR text, LPCSTR texture, int show_time);
    static void OnTalkMessage(LPCSTR caption, LPCSTR text, LPCSTR texture, LPCSTR templ);
    void SendPda(xrClientData* client);
    // The story book (the world actor's info portions, shared by the bodies) to a joining client:
    // "infos|a,b,c" over the Lua channel, in chunks; later transfers reach the clients as the
    // GE_INFO_TRANSFER broadcast (CLevel::cl_Process_Event mirrors them). has_alife_info there.
    void SendWorldInfos(xrClientData* client);
    // Lua channel (M_COOP_LUA): server Lua -> a body's client (target 0/65535 = all clients);
    // client Lua -> coop_server_actor.on_client_lua(body, text) in the body's context.
    static void SendLua(u16 target, LPCSTR text);
    // Sounds the server's Lua and its NPCs play (M_COOP_SOUND), replayed by the clients whose body
    // is within earshot: script sounds (sound_object) not bound to an actor - the players' own
    // presentation is theirs - and the NPC/monster sound player (phrases, cries, monster calls).
    static void RelayScriptSound(u32 sid, LPCSTR path, u32 type, CObject* object, u8 mode, const Fvector* position,
                                 float delay, u32 flags, float volume, float frequency);
    static void RelayScriptSoundStop(u32 sid, bool deferred);
    static void RelayScriptSoundPosition(u32 sid, const Fvector& position);
    static void RelayNpcSound(CObject* object, u32 internal_type, u32 index, u32 max_start, u32 min_start, u32 max_stop,
                              u32 min_stop, LPCSTR prefix, u32 max_count, u32 type, u32 priority, u32 mask, LPCSTR bone);
    // The ray of a shot an NPC fires here (M_COOP_SHOT, CWeapon::FireTrace): the clients' replicas
    // draw the tracer along it, so what the player sees flying at him is the server's bullet.
    static void RelayNpcShot(CObject* shooter, u16 weapon, const Fvector& position, const Fvector& direction);
    void OnLuaMessage(xrClientData* client, NET_Packet& P);
    // Trade with an NPC (M_COOP_TRADE): the server's CTrade of the body and of the NPC do the deal
    // (prices, money, item events); the client only shows the actor menu with the prices sent.
    struct SCoopTrade { u16 npc; };
    xr_map<u32, SCoopTrade> m_trades; // by client id
    void OnTradeMessage(xrClientData* client, NET_Packet& P);
    void TradeStart(xrClientData* client, u16 npc_id);
    static void TradeStartFor(CActor* body, u16 npc_id); // from a dialog line (npc:start_trade) on the server
    // From a dialog line (npc:start_upgrade) on the server: the body's client opens the mechanic
    // window on its replicas ("upgrade|npc" over the Lua channel -> coop_client_actor).
    static void UpgradeStartFor(CActor* body, u16 npc_id);
    void TradeDeal(xrClientData* client, bool buying, const xr_vector<u16>& ids);
    void TradeStop(xrClientData* client);
    void TradeSendPrices(xrClientData* client, u8 op);
    // A body's money changed on the server: the owning client keeps a copy for its menus.
    static void SyncMoney(CInventoryOwner* owner);
    game_sv_Coop();
    virtual ~game_sv_Coop();
    virtual void Update();
    void PrepareClient(xrClientData* client);
    void SendPlayerStore(xrClientData* client); // xrServer::OnCL_Connected, ahead of the connection data
    void ReleaseClient(xrClientData* client);
    virtual LPCSTR type_name() const { return "coop"; }
    virtual void Create(shared_str& options);
    virtual void OnPlayerConnectFinished(ClientID id);
    // Saving is the server's only: the "save" console command of a client (M_SAVE_GAME) or of the
    // server console/autosave saves the ALife world with every body (online ones through the
    // internal client's ClientSave, parked ones from the state kept on their entities) and the
    // Lua state (alife_storage_manager); the players' per-connection store goes to <name>.coopstore.
    // Loading is a server start with the save: coop_server.ltx [server] load = <name>.
    virtual void save_game(NET_Packet&, ClientID);
    // The server console's "load <name>": the world restarts from the save (the internal client
    // reconnects with the rewritten server options); the players are told and reconnect themselves.
    virtual bool load_game(NET_Packet&, ClientID);
    bool m_loaded_save; // the world came from a save (Create)
    static bool IsLoadedSave();
    u32 m_autosave_ms; // [server] autosave_minutes, 0 = off
    u32 m_last_autosave;
    // Level change: one world, everyone travels together. A changer on the server invites the
    // touching body's client (GE_COOP_LEVEL_INVITE -> the SP dialog) or, silent, requests the
    // change itself; the dialog's OK is the SP M_CHANGE_LEVEL. change_level moves every body
    // (connected and parked) and the world actor to the destination, saves the world as
    // coop_level_change and restarts it there (M_CHANGE_LEVEL -> the internal client reconnects);
    // the players' clients reconnect on their own. Others must be within [server]
    // level_change_radius of the requester (default 25 m), else the requester is told to wait.
    static void LevelChangeInvite(CActor* body, NET_Packet& change, bool enabled, const shared_str& invite,
                                  bool has_reject, const Fvector& reject_position, const Fvector& reject_angles);
    static void LevelChangeRequest(CActor* body, NET_Packet& change);
    virtual bool change_level(NET_Packet& net_packet, ClientID sender);
    bool m_changing_level;
    float m_level_change_radius;
    u32 m_level_change_notice; // last "waiting for" notice time
    // Player death: a body that would die goes down instead (CEntity::KillEntity asks DownBody):
    // health pinned at down_health, its client locked to crawling (coop_client_actor), a bleed-out
    // clock ([server] bleedout_seconds). A teammate revives it by "using" the body and staying
    // within revive_range for revive_seconds (health revive_health). The clock running out — or
    // every connected player down at once — kills for real (KillEntity bypassing the intercept):
    // the corpse stays with its inventory, the player gets a new, empty body at the level entry.
    struct SDowned
    {
        u32 since;
        u32 deadline;
        u16 reviver; // body reviving it, u16(-1) none
        u32 revive_since;
        u32 last_notice;
    };
    xr_map<u16, SDowned> m_downed; // by body id
    u32 m_bleedout_ms;
    u32 m_revive_ms;
    float m_revive_range;
    float m_revive_health;
    float m_down_health;
    static bool DownBody(CEntity* entity, u16 who); // true: handled (the body is down, not dead)
    void UpdateDowned();
    void ReviveStart(CActor* reviver, CActor* body);
    void Revive(u16 body_id, SDowned& downed, CActor* body);
    void KillDowned(u16 body_id, SDowned& downed, CActor* body, LPCSTR reason);
    void RespawnClient(xrClientData* client, CActor* corpse);
    CSE_ALifeCreatureActor* SpawnBody(xrClientData* client, bool with_loadout);
    static bool IsDowned(u16 body_id);
    // The character from the join menu (IClient::coop_profile): faction and icon wait for the live
    // body and go on it through coop_server_actor.on_body_profile (game_relations follow the
    // community, the icon is what the PDAs show).
    struct SBodyProfile
    {
        shared_str faction;
        shared_str icon;
    };
    xr_map<u16, SBodyProfile> m_profiles; // by body id, pending
    void UpdateProfiles();
    // Carried items get no regular updates (CInventoryItem::net_Export): the holder's client learns
    // the condition, remaining uses and loaded ammo of its body's items here (GE_COOP_ITEM_STATE)
    // whenever the server's value changes - repairs, workshop verbs, wear, use.
    struct SItemState
    {
        float condition;
        u8 uses;
        u16 ammo;
        u16 place; // SInvItemPlace::value: slot / belt / ruck as the server's inventory has it
    };
    xr_map<u16, SItemState> m_item_states; // by item id, the last state sent
    u32 m_item_states_checked;
    void UpdateItemStates();
    virtual void reload_game(NET_Packet&, ClientID) {}
};
