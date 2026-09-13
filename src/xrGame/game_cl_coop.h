#pragma once
#include "game_cl_single.h"

class game_cl_Coop : public game_cl_Single
{
    typedef game_cl_Single inherited;
    u32 m_probe_started = 0;
    bool m_probe_stopped = false;
    bool m_probe_inventory = false;
    u32 m_probe_last_report = 0;
    bool m_fire_probe_drawn = false;
    u32 m_fire_probe_next = 0;
    u32 m_fire_probe_release = 0;
    u32 m_item_probe_bucket = 0;
    bool m_item_probe_looted = false;
    u32 m_item_probe_last_report = 0;
public:
    virtual void OnConnected();
    virtual void shedule_Update(u32 dt);
    // Per-player Lua state kept by the server (M_COOP_PLAYER_STORE): handed to coop_client_actor.on_player_store.
    void OnPlayerStore(NET_Packet& P);
    // Server-driven NPC dialog (M_COOP_TALK): opens/feeds/closes the talk window in remote mode.
    void OnTalkMessage(NET_Packet& P);
    // The shared PDA (M_COOP_PDA): tasks, map spots and game news mirrored from the server.
    void OnPdaMessage(NET_Packet& P);
    // A text from the server's coop Lua (M_COOP_LUA): handed to coop_client_actor.on_server_lua.
    void OnLuaMessage(NET_Packet& P);
    // Trade with an NPC (M_COOP_TRADE): the server runs the deal, this side shows the actor menu
    // with the prices it sent (CTrade::GetItemPrice asks TradePrice on a coop client).
    xr_map<u16, u32> m_trade_prices;
    u16 m_trade_npc = u16(-1);
    void OnTradeMessage(NET_Packet& P);
    static u32 TradePrice(u16 item_id);
    static void TradeSend(u8 op, u16 npc_or_flag, const xr_vector<u16>* ids);
    // The server restarts its world (level change, console load): this client drops to the menu
    // and reconnects with its own client options until the server answers again (2 minutes).
    static void ScheduleReconnect();
    static void ReconnectUpdate(); // from CGamePersistent::OnFrame, level or no level
    // Downed players (server messages down/revived/died, kept by coop_client_actor through
    // level.coop_set_downed): the own body crawls with no weapon (ActorInput), a teammate's body
    // shows the revive hint and "use" on it asks the server to revive (GE_COOP_USE_OBJECT).
    static void SetDowned(u16 body_id, bool downed);
    static bool IsDowned(u16 body_id);
    static bool SelfDowned(); // the control entity is down
    static void SetReviveHint(LPCSTR text);
    static LPCSTR ReviveHint();
    // Item verbs: the client's Anomaly item scripts (workshop, repair kits, consumables, the
    // mechanic window) change the world through a few engine calls (set_condition, uses, ammo,
    // unload, give_money, give_info, repair). On a coop client such a call goes to the server's
    // coop Lua as "item|<verb>|..." over the Lua channel and runs there on the real objects in
    // this body's context; the replica follows at once for the UI, the server's state comes
    // back (GE_COOP_ITEM_STATE, GE_MONEY, GE_INFO_TRANSFER). Returns false outside a coop client.
    static bool ItemVerb(LPCSTR fmt, ...);
    // Sounds the server relays (M_COOP_SOUND): script sounds by the server's id, NPC/monster
    // sound-player phrases on the replicas (game_sv_Coop::RelayScriptSound / RelayNpcSound).
    xr_map<u32, ref_sound*> m_relayed_sounds;
    void OnSoundMessage(NET_Packet& P);
    void ReleaseRelayedSounds();
    // The rays of the NPC shots the server fired (M_COOP_SHOT, game_sv_Coop::RelayNpcShot), by
    // weapon id: the replica's next tracer takes the oldest fresh one (CAI_Stalker::g_fireParams).
    struct SRelayedShot
    {
        Fvector position, direction;
        u32 time;
    };
    xr_map<u16, xr_deque<SRelayedShot>> m_relayed_shots;
    void OnShotMessage(NET_Packet& P);
    static bool TakeRelayedShot(u16 weapon, Fvector& position, Fvector& direction);
    virtual ~game_cl_Coop();
};
