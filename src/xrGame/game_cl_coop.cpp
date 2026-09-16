#include "stdafx.h"
#include "game_cl_coop.h"
#include "Level.h"
#include "Actor.h"
#include "UIGameSP.h"
#include "../xrEngine/XR_IOConsole.h"
#include "xr_level_controller.h"
#include "ai/stalker/ai_stalker.h"
#include "inventory.h"
#include "weapon.h"
#include "../xrEngine/CameraBase.h"
#include "ai_space.h"
#include "script_engine.h"
#include "ui/UITalkWnd.h"
#include "ui/UIMessagesWindow.h"
#include "InventoryOwner.h"
#include "string_table.h"
#include "GameTask.h"
#include "GametaskManager.h"
#include "map_manager.h"
#include "map_location.h"
#include "game_news.h"
#include "ui/UIActorMenu.h"
#include "MainMenu.h"
#include "CustomMonster.h"
#include "sound_player.h"
#include "coop_alife_mirror.h"
#include "../xrEngine/x_ray.h"

void game_cl_Coop::OnPlayerStore(NET_Packet& P)
{
    // The blob is opaque here; coop_client_actor.script serialises and restores it.
    xr_string blob;
    if (!m_store_parts.receive(P, blob)) return;
    ::luabind::functor<void> functor;
    if (ai().script_engine().functor("coop_client_actor.on_player_store", functor))
        functor(blob.c_str());
    else
        Msg("! [COOP_CLIENT] PLAYER_STORE dropped: coop_client_actor.on_player_store is not loaded");
}

static bool coop_restart_pending = false;
static shared_str coop_restart_level; // the level the server moves the world to (levelchange|<name>)

bool game_cl_Coop::RestartPending()
{
    return coop_restart_pending;
}

void game_cl_Coop::OnLuaMessage(NET_Packet& P)
{
    shared_str text;
    P.r_stringZ(text);
    // The world restarts on the server: from here on the level only waits for the disconnect.
    if (text.c_str() && (!strncmp(text.c_str(), "levelchange|", 12) || !strncmp(text.c_str(), "reload|", 7)))
    {
        coop_restart_pending = true;
        coop_restart_level = !strncmp(text.c_str(), "levelchange|", 12) ? text.c_str() + 12 : "";
    }
    if (text.c_str() && !strncmp(text.c_str(), "reply|", 6))
    {
        u32 request = 0, part = 0, parts = 0; int consumed = 0;
        if (sscanf(text.c_str() + 6, "%u|%u|%u|%n", &request, &part, &parts, &consumed) >= 3 && parts && part >= 1 && part <= parts)
        {
            SReply& reply = m_replies[request];
            if (part == 1) { reply.parts = parts; reply.received = 0; reply.text.clear(); }
            if (part == reply.received + 1) { reply.text += text.c_str() + 6 + consumed; reply.received = part; }
        }
        return;
    }
    if (text.c_str() && !strncmp(text.c_str(), "created|", 8))
    {
        // created|<request>|<id>|<section>|<parent>|x,y,z  (id -1: refused)
        u32 request = 0; int id = -1; string64 section = ""; int parent = -1; Fvector position = { 0, 0, 0 };
        if (sscanf(text.c_str() + 8, "%u|%d|%63[^|]|%d|%f,%f,%f", &request, &id, section, &parent, &position.x, &position.y, &position.z) >= 2)
        {
            SCreateReply& reply = m_create_replies[request];
            reply.id = id < 0 ? u16(-1) : u16(id);
            reply.section = section;
            reply.parent = parent < 0 ? u16(-1) : u16(parent);
            reply.position = position;
        }
        return;
    }
    ::luabind::functor<void> functor;
    if (ai().script_engine().functor("coop_client_actor.on_server_lua", functor))
        functor(text.c_str() ? text.c_str() : "");
    else
        Msg("! [COOP_CLIENT] LUA_MSG dropped: coop_client_actor.on_server_lua is not loaded");
}

static game_cl_Coop* coop_client_game()
{
    if (!IsGameTypeCoop() || OnServer() || !g_pGameLevel || !Level().game) return NULL;
    return static_cast<game_cl_Coop*>(Level().game);
}

LPCSTR game_cl_Coop::WaitReply(u32 request, u32 timeout_ms)
{
    static xr_string last;
    game_cl_Coop* game = coop_client_game();
    if (!game) return NULL;
    const u32 started = GetTickCount();
    xr_map<u32, SReply>::iterator it = game->m_replies.find(request);
    while (it == game->m_replies.end() || it->second.received < it->second.parts)
    {
        if (GetTickCount() - started >= timeout_ms || Level().net_isDisconnected()) break;
        Level().Flush_Send_Buffer();
        Level().ClientReceive();
        Sleep(1);
        it = game->m_replies.find(request);
    }
    if (it == game->m_replies.end() || it->second.received < it->second.parts)
    {
        Msg("! [COOP_CLIENT] RPC request=%u: no answer in %u ms", request, timeout_ms);
        if (it != game->m_replies.end()) game->m_replies.erase(it);
        return NULL;
    }
    last = it->second.text;
    game->m_replies.erase(it);
    if (strstr(Core.Params, "-coop_damage_probe"))
        Msg("[COOP_CLIENT] RPC request=%u bytes=%u waited=%u ms", request, u32(last.size()), GetTickCount() - started);
    return last.c_str();
}

int game_cl_Coop::CreateWait(u32 request, u32 timeout_ms)
{
    game_cl_Coop* game = coop_client_game();
    if (!game) return -1;
    const u32 started = GetTickCount();
    xr_map<u32, SCreateReply>::iterator it = game->m_create_replies.find(request);
    while (it == game->m_create_replies.end())
    {
        if (GetTickCount() - started >= timeout_ms || Level().net_isDisconnected()) break;
        Level().Flush_Send_Buffer(); // the request sits in the multipacket buffer until a frame flushes it
        Level().ClientReceive(); // the reply and, with luck, the object's spawn; other messages run their Lua meanwhile
        Sleep(1);
        it = game->m_create_replies.find(request);
    }
    if (it == game->m_create_replies.end())
    {
        Msg("! [COOP_CLIENT] CREATE_WAIT request=%u: no answer in %u ms", request, timeout_ms);
        return -1;
    }
    SCreateReply reply = it->second;
    game->m_create_replies.erase(it);
    if (reply.id == u16(-1)) return -1;
    CCoopAlifeMirror* mirror = CCoopAlifeMirror::instance();
    if (mirror && !mirror->entity(reply.id)) mirror->placeholder(reply.id, reply.section.c_str(), reply.parent, reply.position);
    if (strstr(Core.Params, "-coop_damage_probe"))
        Msg("[COOP_CLIENT] CREATE_WAIT request=%u id=%u section=%s parent=%u waited=%u ms", request, reply.id, reply.section.c_str(), reply.parent, GetTickCount() - started);
    return int(reply.id);
}

u32 game_cl_Coop::TradePrice(u16 item_id)
{
    game_cl_Coop* game = coop_client_game();
    if (!game) return 0;
    xr_map<u16, u32>::const_iterator it = game->m_trade_prices.find(item_id);
    return it == game->m_trade_prices.end() ? 0 : it->second;
}

void game_cl_Coop::TradeSend(u8 op, u16 npc_or_flag, const xr_vector<u16>* ids)
{
    if (!coop_client_game()) return;
    NET_Packet P;
    P.w_begin(M_COOP_TRADE);
    P.w_u8(op);
    if (op == 1) P.w_u16(npc_or_flag);
    if (op == 2)
    {
        P.w_u8(u8(npc_or_flag));
        const u32 count = ids ? _min(u32(ids->size()), u32(255)) : 0;
        P.w_u8(u8(count));
        for (u32 i = 0; i < count; ++i) P.w_u16((*ids)[i]);
    }
    Level().Send(P, net_flags(TRUE, TRUE));
}

// ---- reconnect after a server world restart ------------------------------------------------------

static struct
{
    xr_string client_options;
    u32 next_attempt;
    u32 deadline;
    bool pending;
    bool screen_set; // the wait screen's texts and picture are up
} coop_reconnect = { "", 0, 0, false, false };

extern ENGINE_API LOADING_EVENT g_disconnect_wait; // x_ray.cpp

// The disconnect that follows shows the loading screen with ReconnectWaitEvent under it instead
// of the main menu and its "connection closed" box.
static void coop_arm_wait_screen()
{
    g_disconnect_wait.bind(&game_cl_Coop::ReconnectWaitEvent);
    coop_reconnect.screen_set = false;
}

void game_cl_Coop::ScheduleReconnect()
{
    if (!g_pGameLevel || !Level().m_caClientOptions.c_str()) return;
    coop_reconnect.client_options = Level().m_caClientOptions.c_str();
    coop_reconnect.pending = !coop_reconnect.client_options.empty();
    coop_reconnect.next_attempt = Device.TimerAsync() + 20000; // the server reloads its world first
    coop_reconnect.deadline = Device.TimerAsync() + 180000;
    coop_restart_pending = true;
    coop_arm_wait_screen();
    Msg("[COOP_CLIENT] RECONNECT scheduled options=%s", coop_reconnect.client_options.c_str());
}

// Runs once a frame from the loading-event queue while there is no level (CRenderDevice::on_idle
// draws the loading screen meanwhile): the next connection attempt when it is due, the menu when
// the reconnect is over. A failed attempt disconnects again and comes back here (re-armed).
bool game_cl_Coop::ReconnectWaitEvent()
{
    if (!coop_reconnect.screen_set)
    {
        coop_reconnect.screen_set = true;
        if (coop_restart_level.size()) pApp->LoadLevelLogo(coop_restart_level.c_str());
        pApp->LoadTitleInt(CStringTable().translate("ls_header").c_str(),
                           coop_restart_level.size() ? CStringTable().translate(coop_restart_level).c_str() : "",
                           CStringTable().translate("st_coop_wait_server").c_str());
    }
    // Device.dwTimeGlobal stands still while loading events run (no FrameMove): the wall clock.
    const u32 now = Device.TimerAsync();
    if (coop_reconnect.pending && now > coop_reconnect.deadline)
    {
        coop_reconnect.pending = false;
        Msg("! [COOP_CLIENT] RECONNECT gave up");
    }
    if (!coop_reconnect.pending)
    {
        pApp->LoadEnd();
        Console->Execute("main_menu on");
        return true;
    }
    if (now < coop_reconnect.next_attempt) return false;
    coop_reconnect.next_attempt = now + 10000;
    coop_arm_wait_screen();
    pApp->LoadEnd(); // the start's own LoadBegin carries the screen on
    string1024 command;
    xr_sprintf(command, "start client(%s)", coop_reconnect.client_options.c_str());
    Msg("[COOP_CLIENT] RECONNECT attempt: %s", command);
    Console->Execute(command);
    return true;
}

void game_cl_Coop::ReconnectUpdate()
{
    if (!coop_reconnect.pending) return;
    if (g_pGameLevel)
    {
        // An attempt in progress, or back in the game: done once the level is ready.
        if (g_pGameLevel->bReady && Level().game && !Level().net_isDisconnected())
        {
            coop_reconnect.pending = false;
            g_disconnect_wait.clear(); // a later disconnect is a real one: the menu again
            Msg("[COOP_CLIENT] RECONNECTED");
        }
        return;
    }
    if (!g_loading_events.empty() || !g_disconnect_wait.empty()) return; // the wait screen runs the attempts
    if (Device.TimerAsync() > coop_reconnect.deadline)
    {
        coop_reconnect.pending = false;
        Msg("! [COOP_CLIENT] RECONNECT gave up");
        return;
    }
    if (Device.TimerAsync() < coop_reconnect.next_attempt) return;
    coop_reconnect.next_attempt = Device.TimerAsync() + 10000;
    string1024 command;
    xr_sprintf(command, "start client(%s)", coop_reconnect.client_options.c_str());
    Msg("[COOP_CLIENT] RECONNECT attempt: %s", command);
    coop_heap_check("reconnect attempt");
    // As a load from the SP main menu (CCC_ALifeLoadFrom): the menu the disconnect opened goes
    // before the level comes — loaded under it, the level stayed behind the menu (120).
    if (MainMenu() && MainMenu()->IsActive()) MainMenu()->Activate(false);
    Console->Execute(command);
}

// ---- downed players -------------------------------------------------------------------------------

bool game_cl_Coop::s_following_place = false;

static xr_set<u16> coop_downed_bodies;
static xr_string coop_revive_hint = "Revive";

void game_cl_Coop::SetDowned(u16 body_id, bool downed)
{
    if (downed) coop_downed_bodies.insert(body_id); else coop_downed_bodies.erase(body_id);
}

bool game_cl_Coop::IsDowned(u16 body_id)
{
    return coop_downed_bodies.find(body_id) != coop_downed_bodies.end();
}

bool game_cl_Coop::SelfDowned()
{
    if (!IsGameTypeCoop() || OnServer() || !g_pGameLevel || coop_downed_bodies.empty()) return false;
    CObject* control = Level().CurrentControlEntity();
    return control && IsDowned(control->ID());
}

void game_cl_Coop::SetReviveHint(LPCSTR text)
{
    coop_revive_hint = text ? text : "";
}

LPCSTR game_cl_Coop::ReviveHint()
{
    return coop_revive_hint.c_str();
}

void g_coop_send_lua(u16 target, LPCSTR text); // level_script.cpp

game_cl_Coop::~game_cl_Coop()
{
    ReleaseRelayedSounds();
}

void game_cl_Coop::OnShotMessage(NET_Packet& P)
{
    if (OnServer() || P.B.count < P.r_tell() + sizeof(u16) + 2 * sizeof(Fvector)) return;
    SRelayedShot shot;
    const u16 weapon = P.r_u16();
    P.r_vec3(shot.position);
    P.r_vec3(shot.direction);
    shot.time = Device.dwTimeGlobal;
    xr_deque<SRelayedShot>& queue = m_relayed_shots[weapon];
    queue.push_back(shot);
    while (queue.size() > 8) queue.pop_front();
}

bool game_cl_Coop::TakeRelayedShot(u16 weapon, Fvector& position, Fvector& direction)
{
    if (!IsGameTypeCoop() || OnServer() || !g_pGameLevel) return false;
    game_cl_Coop* game = smart_cast<game_cl_Coop*>(&Game());
    if (!game) return false;
    xr_map<u16, xr_deque<SRelayedShot>>::iterator it = game->m_relayed_shots.find(weapon);
    if (it == game->m_relayed_shots.end()) return false;
    xr_deque<SRelayedShot>& queue = it->second;
    while (!queue.empty() && Device.dwTimeGlobal - queue.front().time > 1000) queue.pop_front(); // stale: the burst is over
    if (queue.empty()) return false;
    position = queue.front().position;
    direction = queue.front().direction;
    queue.pop_front();
    return true;
}

void game_cl_Coop::ReleaseRelayedSounds()
{
    for (xr_map<u32, ref_sound*>::iterator it = m_relayed_sounds.begin(); it != m_relayed_sounds.end(); ++it)
    {
        ref_sound* sound = it->second;
        if (sound->_feedback()) sound->stop();
        sound->destroy();
        xr_delete(sound);
    }
    m_relayed_sounds.clear();
}

void game_cl_Coop::OnSoundMessage(NET_Packet& P)
{
    if (OnServer() || P.B.count < P.r_tell() + sizeof(u8)) return;
    const u8 op = P.r_u8();
    switch (op)
    {
    case 1: // a script sound (sound_object) of the server's Lua
    {
        const u32 sid = P.r_u32();
        shared_str path;
        P.r_stringZ(path);
        const u32 type = P.r_u32();
        const u16 object_id = P.r_u16();
        const u8 mode = P.r_u8();
        Fvector position;
        P.r_vec3(position);
        const float delay = P.r_float();
        const u32 flags = P.r_u32();
        float volume = P.r_float();
        float frequency = P.r_float();
        CObject* object = object_id != u16(-1) ? Level().Objects.net_Find(object_id) : NULL;
        if (object_id != u16(-1) && !object) return; // its object is not here: out of range anyway
        ref_sound*& sound = m_relayed_sounds[sid];
        if (!sound)
        {
            string_path file;
            if (!FS.exist(file, "$game_sounds$", path.c_str(), ".ogg"))
            {
                m_relayed_sounds.erase(sid);
                return;
            }
            sound = xr_new<ref_sound>();
            sound->create(path.c_str(), st_Effect, ESoundTypes(type));
        }
        else if (sound->_feedback())
            sound->stop();
        if (!sound->_handle()) return;
        if (mode == 0) sound->play(object, flags, delay);
        else if (mode == 1) sound->play_at_pos(object, position, flags, delay);
        else sound->play_no_feedback(object, flags, delay, &position, &volume, &frequency);
        break;
    }
    case 2:
    {
        const u32 sid = P.r_u32();
        const bool deferred = !!P.r_u8();
        xr_map<u32, ref_sound*>::iterator it = m_relayed_sounds.find(sid);
        if (it == m_relayed_sounds.end() || !it->second->_feedback()) return;
        if (deferred) it->second->stop_deffered(); else it->second->stop();
        break;
    }
    case 3:
    {
        const u32 sid = P.r_u32();
        Fvector position;
        P.r_vec3(position);
        xr_map<u32, ref_sound*>::iterator it = m_relayed_sounds.find(sid);
        if (it != m_relayed_sounds.end() && it->second->_feedback()) it->second->set_position(position);
        break;
    }
    case 4: // the NPC/monster sound player: the same collection and index on the replica
    {
        const u16 object_id = P.r_u16();
        const u32 internal_type = P.r_u32();
        const u32 index = P.r_u32();
        const u32 max_start = P.r_u32();
        const u32 min_start = P.r_u32();
        const u32 max_stop = P.r_u32();
        const u32 min_stop = P.r_u32();
        shared_str prefix, bone;
        P.r_stringZ(prefix);
        const u32 max_count = P.r_u32();
        const u32 type = P.r_u32();
        const u32 priority = P.r_u32();
        const u32 mask = P.r_u32();
        P.r_stringZ(bone);
        CCustomMonster* monster = smart_cast<CCustomMonster*>(Level().Objects.net_Find(object_id));
        if (!monster || monster->getDestroy() || !monster->Visual()) return;
        monster->sound().add(prefix.c_str(), max_count, ESoundTypes(type), priority, mask, internal_type, bone.c_str(), NULL);
        monster->sound().play(internal_type, max_start, min_start, max_stop, min_stop, index);
        break;
    }
    default:
        break;
    }
}

bool game_cl_Coop::ItemVerb(LPCSTR fmt, ...)
{
    if (!IsGameTypeCoop() || !g_pGameLevel || !OnClient()) return false;
    string1024 text;
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    text[sizeof(text) - 1] = 0;
    if (strstr(Core.Params, "-coop_damage_probe")) Msg("[COOP_ITEM_VERB] %s", text);
    g_coop_send_lua(0, text);
    return true;
}

void game_cl_Coop::OnTradeMessage(NET_Packet& P)
{
    CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
    CActor* actor = smart_cast<CActor*>(Level().CurrentControlEntity());
    if (!ui || !actor) return;
    const u8 op = P.r_u8();
    switch (op)
    {
    case 1: // begin + prices
    case 2: // prices after a deal
    {
        const u16 npc_id = P.r_u16();
        const u32 npc_money = P.r_u32();
        const u16 count = P.r_u16();
        m_trade_prices.clear();
        for (u16 i = 0; i < count && P.B.count >= P.r_tell() + sizeof(u16) + sizeof(u32); ++i)
        {
            const u16 id = P.r_u16();
            const u32 price = P.r_u32();
            m_trade_prices[id] = price;
        }
        m_trade_npc = npc_id;
        CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(npc_id));
        if (npc) npc->set_money(npc_money, false); // the menu's "can the NPC pay" check and its money label
        if (op == 1)
        {
            if (npc) ui->StartTrade(actor, npc);
        }
        else if (ui->GetActorMenu().IsShown())
            ui->GetActorMenu().CoopPricesChanged();
        if (strstr(Core.Params, "-coop_damage_probe"))
            Msg("[COOP_CLIENT] TRADE_PRICES op=%u npc=%u count=%u", u32(op), npc_id, u32(count));
        break;
    }
    case 4: // refused
    {
        const u8 reason = P.r_u8();
        Msg("[COOP_CLIENT] TRADE_REFUSED reason=%u", u32(reason));
        if (reason == 5 && ui->m_pMessagesWnd) ui->m_pMessagesWnd->AddChatMessage(CStringTable().translate("not_enough_money_actor"), "");
        break;
    }
    default:
        break;
    }
}

void game_cl_Coop::OnTalkMessage(NET_Packet& P)
{
    CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
    CActor* actor = Actor();
    if (!ui || !ui->TalkMenu || !actor || P.B.count < P.r_tell() + sizeof(u8)) return;
    const u8 op = P.r_u8();
    switch (op)
    {
    case 1: // begin: the server accepted the request and both sides are talking there
    {
        const u16 npc_id = P.r_u16();
        const bool disable_break = !!P.r_u8();
        CGameObject* npc_object = smart_cast<CGameObject*>(Level().Objects.net_Find(npc_id));
        CInventoryOwner* npc = smart_cast<CInventoryOwner*>(npc_object);
        if (!npc || actor->IsTalking()) return;
        // Replica flags only: CInventoryOwner::UpdateInventoryOwner ends a talk whose partner is silent.
        npc->StartTalk(actor);
        actor->StartTalk(npc);
        if (ui->TopInputReceiver()) ui->TopInputReceiver()->HideDialog();
        ui->TalkMenu->SetRemote(true);
        ui->StartTalk(disable_break);
        Msg("[COOP_CLIENT] TALK_BEGIN npc=%u", npc_id);
        break;
    }
    case 2: // answer
    {
        const bool ours = !!P.r_u8();
        shared_str text;
        P.r_stringZ(text);
        if (ui->TalkMenu->IsShown() && ui->TalkMenu->IsRemote())
            ui->TalkMenu->RemoteAnswer(ours, text.c_str());
        break;
    }
    case 3: // questions
    {
        const bool topic_mode = !!P.r_u8();
        const u8 count = P.r_u8();
        const bool shown = ui->TalkMenu->IsShown() && ui->TalkMenu->IsRemote();
        if (shown) ui->TalkMenu->RemoteClearQuestions();
        for (u8 i = 0; i < count && P.B.count > P.r_tell(); ++i)
        {
            shared_str text, id;
            P.r_stringZ(text);
            P.r_stringZ(id);
            const bool finalizer = !!P.r_u8();
            if (shown) ui->TalkMenu->AddQuestion(text, id, i, finalizer);
        }
        if (strstr(Core.Params, "-coop_damage_probe"))
            Msg("[COOP_CLIENT] TALK_QUESTIONS topic=%u count=%u shown=%u", topic_mode ? 1 : 0, count, shown ? 1 : 0);
        break;
    }
    case 4: // end
    {
        if (ui->TalkMenu->IsShown()) ui->TalkMenu->HideDialog();
        else if (actor->IsTalking()) actor->StopTalk();
        break;
    }
    case 5: // another player's conversation: a game news with the speaker's portrait and the phrase
    {
        shared_str speaker, text, icon;
        P.r_stringZ(speaker);
        P.r_stringZ(text);
        P.r_stringZ(icon);
        if (!g_actor) break;
        GAME_NEWS_DATA news;
        news.m_type = GAME_NEWS_DATA::eTalk;
        news.news_caption = speaker;
        news.news_text = CStringTable().translate(text);
        news.texture_name = icon.size() ? icon : shared_str("ui_iconsTotal_grouping");
        Actor()->AddGameNews(news);
        break;
    }
    }
}

void game_cl_Coop::OnConnected()
{
    inherited::OnConnected();
    coop_restart_pending = false;
    coop_downed_bodies.clear(); // a fresh world: the server tells again who is down
    ReleaseRelayedSounds();
    m_relayed_shots.clear();
    Msg("[COOP_BOOTSTRAP] client connected; server_side=%d", OnServer() ? 1 : 0);
    if (OnServer() && strstr(Core.Params, "-coop_missile_ui_probe"))
    {
        extern void create_force_progress();
        create_force_progress();
        Msg("[COOP_MISSILE_UI] HEADLESS_CREATE_RETURNED");
        FlushLog();
    }
    if (!OnServer()) m_probe_started = Device.dwTimeGlobal;
}


void game_cl_Coop::shedule_Update(u32 dt)
{
    inherited::shedule_Update(dt);
    if (OnServer() || !m_probe_started || m_probe_stopped || !strstr(Core.Params, "-coop_client_probe")) return;
    const u32 elapsed = Device.dwTimeGlobal - m_probe_started;
    CActor* actor = smart_cast<CActor*>(Level().CurrentControlEntity());
    if (actor && elapsed - m_probe_last_report >= 1000)
    {
        m_probe_last_report = elapsed;
        Msg("[COOP_CLIENT] PROBE_STATE elapsed=%u body=%u local=%d alive=%d health=%f enabled=%d ready=%d position=%f,%f,%f",
            elapsed, actor->ID(), actor->Local(), actor->g_Alive(), actor->GetfHealth(), actor->getEnabled(), actor->Ready(), VPUSH(actor->Position()));
        FlushLog();
    }
    if (actor && elapsed >= 5000 && elapsed < 15000) actor->IR_OnKeyboardHold(kFWD);
    if (actor && elapsed >= 10000 && elapsed < 15000 && !m_probe_inventory && strstr(Core.Params, "-coop_inventory_probe"))
    {
        CurrentGameUI()->IR_UIOnKeyboardPress(get_action_dik(kINVENTORY));
        m_probe_inventory = true;
        Msg("[COOP_CLIENT] INVENTORY_OPEN_REQUEST body=%u", actor->ID());
    }
    if (m_probe_inventory && elapsed >= 15000 && elapsed < 17000)
    {
        CurrentGameUI()->IR_UIOnKeyboardPress(get_action_dik(kINVENTORY));
        m_probe_inventory = false;
    }
    if (actor && strstr(Core.Params, "-coop_fire_probe") && actor->g_Alive())
    {
        // Diagnostic only: make sure the rifle is drawn, then click-fire at the nearest live stalker.
        if (elapsed >= 20000 && !m_fire_probe_drawn)
        {
            m_fire_probe_drawn = true;
            // The loadout rifle usually auto-activates on pickup; pressing the slot key again would hide it.
            if (actor->inventory().GetActiveSlot() != INV_SLOT_3) actor->IR_OnKeyboardPress(kWPN_3);
            Msg("[COOP_FIRE_PROBE] DRAW body=%u slot=%u", actor->ID(), actor->inventory().GetActiveSlot());
        }
        if (elapsed >= 24000 && elapsed < 40000)
        {
            CAI_Stalker* target = NULL;
            float best = 40.f;
            for (u32 n = 0; n < Level().Objects.o_count(); ++n)
            {
                CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().Objects.o_get_by_iterator(n));
                if (!stalker || !stalker->g_Alive()) continue;
                const float distance = stalker->Position().distance_to(actor->Position());
                if (distance < best) { best = distance; target = stalker; }
            }
            if (target)
            {
                // Same look-at math as CCameraFirstEye::UpdateLookat.
                Fvector to; target->Center(to);
                Fvector dir; dir.sub(to, actor->cam_Active()->Position());
                Fmatrix m; m.identity(); m.k.normalize_safe(dir);
                Fvector::generate_orthonormal_basis(m.k, m.j, m.i);
                Fvector xyz; m.getXYZi(xyz);
                actor->cam_Active()->yaw = xyz.y;
                actor->cam_Active()->pitch = xyz.x;
                if (elapsed >= m_fire_probe_next)
                {
                    m_fire_probe_next = elapsed + 2500;
                    m_fire_probe_release = elapsed + 1; // a click: released on the next scheduled update
                    CWeapon* weapon = smart_cast<CWeapon*>(actor->inventory().ActiveItem());
                    actor->IR_OnKeyboardPress(kWPN_FIRE);
                    Msg("[COOP_FIRE_PROBE] FIRE body=%u target=%u distance=%f slot=%u weapon=%u ammo=%d aim=%f,%f,%f eye=%f,%f,%f",
                        actor->ID(), target->ID(), best, actor->inventory().GetActiveSlot(), weapon ? weapon->ID() : 0,
                        weapon ? weapon->GetAmmoElapsed() : -1, VPUSH(actor->cam_Active()->Direction()), VPUSH(actor->cam_Active()->Position()));
                }
            }
            else if (elapsed >= m_fire_probe_next)
            {
                m_fire_probe_next = elapsed + 2500;
                Msg("[COOP_FIRE_PROBE] NO_TARGET body=%u", actor->ID());
            }
        }
        if (m_fire_probe_release && elapsed >= m_fire_probe_release)
        {
            m_fire_probe_release = 0;
            actor->IR_OnKeyboardRelease(kWPN_FIRE);
        }
    }
    if (actor && strstr(Core.Params, "-coop_item_probe") && actor->g_Alive() && elapsed >= 20000)
    {
        // Diagnostic only. Both clients ask for the same free medkit at the same server-time
        // boundary, so the server has to pick exactly one taker. Once, an item is looted from a corpse.
        const u32 bucket = Level().timeServer() / 10000;
        if (bucket != m_item_probe_bucket)
        {
            m_item_probe_bucket = bucket;
            CInventoryItem* nearest = NULL;
            float best = 3.f;
            for (u32 n = 0; n < Level().Objects.o_count(); ++n)
            {
                CGameObject* object = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(n));
                CInventoryItem* item = object ? object->cast_inventory_item() : NULL;
                if (!item || object->H_Parent() || object->getDestroy() || xr_strcmp(object->cNameSect().c_str(), "medkit")) continue;
                const float distance = object->Position().distance_to(actor->Position());
                if (distance < best) { best = distance; nearest = item; }
            }
            if (nearest)
            {
                Game().SendPickUpEvent(actor->ID(), nearest->object().ID());
                Msg("[COOP_ITEM_PROBE] TAKE body=%u item=%u distance=%f time=%u", actor->ID(), nearest->object().ID(), best, Level().timeServer());
            }
        }
        if (elapsed >= 45000 && !m_item_probe_looted)
        {
            CAI_Stalker* corpse = NULL;
            float best = 3.5f;
            for (u32 n = 0; n < Level().Objects.o_count(); ++n)
            {
                CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().Objects.o_get_by_iterator(n));
                if (!stalker || stalker->g_Alive() || stalker->inventory().m_all.empty()) continue;
                const float distance = stalker->Position().distance_to(actor->Position());
                if (distance < best) { best = distance; corpse = stalker; }
            }
            if (corpse)
            {
                m_item_probe_looted = true;
                CInventoryItem* item = corpse->inventory().m_all.front();
                NET_Packet P;
                CGameObject::u_EventGen(P, GE_TRADE_SELL, corpse->ID());
                P.w_u16(item->object().ID());
                CGameObject::u_EventSend(P);
                CGameObject::u_EventGen(P, GE_TRADE_BUY, actor->ID());
                P.w_u16(item->object().ID());
                CGameObject::u_EventSend(P);
                Msg("[COOP_ITEM_PROBE] LOOT body=%u corpse=%u item=%u section=%s distance=%f", actor->ID(), corpse->ID(), item->object().ID(),
                    item->object().cNameSect().c_str(), best);
            }
        }
        if (elapsed - m_item_probe_last_report >= 1000)
        {
            m_item_probe_last_report = elapsed;
            u32 medkits = 0;
            for (u32 n = 0; n < Level().Objects.o_count(); ++n)
            {
                CGameObject* object = smart_cast<CGameObject*>(Level().Objects.o_get_by_iterator(n));
                if (!object || xr_strcmp(object->cNameSect().c_str(), "medkit")) continue;
                Msg("[COOP_ITEM_PROBE] side=client body=%u item=%u parent=%u", actor->ID(), object->ID(), object->H_Parent() ? object->H_Parent()->ID() : u16(-1));
                if (object->H_Parent() == actor) ++medkits;
            }
            Msg("[COOP_ITEM_PROBE] INVENTORY body=%u items=%u medkits=%u", actor->ID(), u32(actor->inventory().m_all.size()), medkits);
        }
    }
    if (strstr(Core.Params, "-coop_client_stall_probe") && elapsed >= 30000)
    {
        static bool stalled = false;
        if (!stalled)
        {
            stalled = true;
            Msg("[COOP_STALL] BEGIN time=%u", Level().timeServer());
            FlushLog();
            Sleep(5000);
            Msg("[COOP_STALL] END time=%u", Level().timeServer());
        }
    }
    const u32 duration = strstr(Core.Params, "-coop_client_long_probe") ? 90000 :
        (strstr(Core.Params, "-coop_spatial_client_probe") ? 45000 : 20000);
    if (elapsed >= duration)
    {
        R_ASSERT(actor && actor->Local());
        Msg("[COOP_CLIENT] PROBE_DONE body=%u position=%f,%f,%f", actor->ID(), VPUSH(actor->Position()));
        m_probe_stopped = true;
        Console->Execute("quit");
    }
}

void game_cl_Coop::OnPdaMessage(NET_Packet& P)
{
    const u8 op = P.r_u8();
    switch (op)
    {
    case 1: // task: create or refresh the local copy of the server's task
    {
        shared_str id, title, descr, icon, map_location, map_hint;
        P.r_stringZ(id);
        const u8 type = P.r_u8();
        const u8 state = P.r_u8();
        const u32 priority = P.r_u32();
        P.r_stringZ(title);
        P.r_stringZ(descr);
        P.r_stringZ(icon);
        P.r_stringZ(map_location);
        const u16 map_object_id = P.r_u16();
        P.r_stringZ(map_hint);
        const bool has_where = P.r_u8() != 0;
        shared_str where_level;
        Fvector where_position;
        where_position.set(0.f, 0.f, 0.f);
        if (has_where)
        {
            P.r_stringZ(where_level);
            P.r_vec3(where_position);
        }
        if (!id.size()) break;
        CGameTaskManager& manager = Level().GameTaskManager();
        CGameTask* task = manager.HasGameTask(id, false);
        const bool fresh = !task;
        if (fresh)
        {
            task = xr_new<CGameTask>();
            task->m_ID = id;
        }
        task->SetType_script(type);
        task->m_priority = priority;
        task->m_Title = title;
        task->m_Description = descr;
        task->m_icon_texture_name = icon;
        task->m_map_hint = map_hint;
        if (fresh)
        {
            // The copy carries no infos or functors: its state only changes by this message.
            task->m_map_location = map_location;
            task->m_map_object_id = map_object_id;
            manager.GiveGameTaskToActor(task, 0, false, 0);
        }
        else if (task->GetTaskState() == eTaskStateInProgress && state == eTaskStateInProgress &&
                 (task->m_map_location != map_location || task->m_map_object_id != map_object_id))
            task->ChangeMapLocation(map_location.c_str() ? map_location.c_str() : "", map_object_id);
        // The target may live on another level: the server says where, the spot draws there.
        if (has_where && task->LinkedMapLocation())
            task->LinkedMapLocation()->InitCoopExternal(where_level.c_str(), where_position);
        if (task->GetTaskState() != ETaskState(state) && state != eTaskStateInProgress)
            manager.SetTaskState(task, ETaskState(state));
        if (strstr(Core.Params, "-coop_damage_probe"))
            Msg("[COOP_CLIENT] PDA_TASK id=%s fresh=%u state=%u map=%s/%u", id.c_str(), fresh ? 1 : 0, u32(state),
                map_location.c_str() ? map_location.c_str() : "", map_object_id);
        break;
    }
    case 2: // map spot add
    {
        shared_str spot, hint;
        P.r_stringZ(spot);
        const u16 id = P.r_u16();
        P.r_stringZ(hint);
        const u8 serializable = P.r_u8();
        const bool has_where = P.r_u8() != 0;
        shared_str where_level;
        Fvector where_position;
        where_position.set(0.f, 0.f, 0.f);
        if (has_where)
        {
            P.r_stringZ(where_level);
            P.r_vec3(where_position);
        }
        if (!spot.size()) break;
        CMapLocation* ml = Level().MapManager().GetMapLocation(spot, id);
        if (!ml) ml = Level().MapManager().AddMapLocation(spot, id);
        if (hint.size()) ml->SetHint(hint);
        if (serializable) ml->SetSerializable(true);
        if (has_where) ml->InitCoopExternal(where_level.c_str(), where_position);
        break;
    }
    case 3: // map spot hint
    {
        shared_str spot, hint;
        P.r_stringZ(spot);
        const u16 id = P.r_u16();
        P.r_stringZ(hint);
        CMapLocation* ml = spot.size() ? Level().MapManager().GetMapLocation(spot, id) : NULL;
        if (ml) ml->SetHint(hint.c_str() ? hint.c_str() : "");
        break;
    }
    case 4: // map spot remove
    {
        shared_str spot;
        P.r_stringZ(spot);
        const u16 id = P.r_u16();
        if (spot.size()) Level().MapManager().RemoveMapLocation(spot, id);
        break;
    }
    case 5: // map spots remove by object
    {
        const u16 id = P.r_u16();
        Level().MapManager().RemoveAllMapLocationsById(id);
        break;
    }
    case 6: // game news
    {
        GAME_NEWS_DATA news;
        news.m_type = GAME_NEWS_DATA::eNewsType(P.r_u8());
        P.r_stringZ(news.news_caption);
        P.r_stringZ(news.news_text);
        P.r_stringZ(news.texture_name);
        const s32 show_time = P.r_s32();
        if (show_time) news.show_time = show_time;
        if (!news.texture_name.size()) news.texture_name = "ui_iconsTotal_grouping";
        if (g_actor) Actor()->AddGameNews(news);
        break;
    }
    case 7: // iconed message inside the talk window (a task given in a dialog)
    {
        shared_str caption, text, texture, templ;
        P.r_stringZ(caption);
        P.r_stringZ(text);
        P.r_stringZ(texture);
        P.r_stringZ(templ);
        CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
        if (ui && ui->TalkMenu && ui->TalkMenu->IsShown())
            ui->TalkMenu->AddIconedMessage(caption.c_str(), text.c_str(), texture.c_str(), templ.size() ? templ.c_str() : "iconed_answer_item");
        else if (g_actor)
        {
            GAME_NEWS_DATA news;
            news.m_type = GAME_NEWS_DATA::eTalk;
            news.news_caption = caption;
            news.news_text = text;
            news.texture_name = texture.size() ? texture : shared_str("ui_iconsTotal_grouping");
            Actor()->AddGameNews(news);
        }
        break;
    }
    default:
        Msg("! [COOP_CLIENT] unknown PDA op %u", u32(op));
        break;
    }
}
