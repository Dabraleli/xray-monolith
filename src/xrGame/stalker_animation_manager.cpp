////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager.cpp
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_animation_data_storage.h"
#include "stalker_animation_data.h"
#include "stalker_movement_manager_smart_cover.h"

#include "CharacterPhysicsSupport.h"
#include "sight_manager.h"
#include "level.h"
#include "inventory.h"
#include "inventory_item.h"
#include "HudItem.h"
#include "object_handler_space.h"
#include "object_handler_planner.h"

// TODO:
// stalker animation manager consists of 5 independent managers,
// they should be represented with the different classes:
//    * head
//    * torso
//    * legs
//    * globals
//    * script

CStalkerAnimationManager::CStalkerAnimationManager(CAI_Stalker* object) :
	m_object(object),
	m_global(object),
	m_head(object),
	m_torso(object),
	m_legs(object),
	m_script(object),
	m_start_new_script_animation(false)
{
}

void CStalkerAnimationManager::reinit()
{
	m_direction_start = 0;
    for (u32 i=0;i<3;++i) m_network_rotations[i].identity();
	m_current_direction = eMovementDirectionForward;
    if (IsGameTypeCoop()) m_previous_speed_direction = eMovementDirectionForward;
	m_target_direction = eMovementDirectionForward;

	m_change_direction_time = 0;
	m_looking_back = 0;

	m_no_move_actual = false;

	m_script_animations.clear();

	m_global.reset();
	m_head.reset();
	m_torso.reset();
	m_legs.reset();
	m_script.reset();

	m_legs.step_dependence(true);
	m_global.step_dependence(true);
	m_script.step_dependence(true);

	m_global.global_animation(true);
	m_script.global_animation(true);

	m_call_script_callback = false;

	m_previous_speed = 0.f;
	m_target_speed = 0.f;
	m_last_non_zero_speed = m_target_speed;

	m_special_danger_move = false;
}

void CStalkerAnimationManager::reload()
{
	m_visual = object().Visual();

	m_crouch_state_config = object().SpecificCharacter().crouch_type();
	VERIFY((m_crouch_state_config == 0) || (m_crouch_state_config == 1) || (m_crouch_state_config == -1));
	m_crouch_state = m_crouch_state_config;

	if (object().already_dead())
		return;

	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_visual);
	VERIFY(m_skeleton_animated);

	m_data_storage = stalker_animation_data_storage().object(m_skeleton_animated);
	VERIFY(m_data_storage);

	if (!object().g_Alive())
		return;

#ifdef USE_HEAD_BONE_PART_FAKE
	VERIFY(!m_data_storage->m_head_animations.A.empty());
	u16 bone_part = m_skeleton_animated->LL_GetMotionDef(m_data_storage->m_head_animations.A.front())->bone_or_part;
	VERIFY(bone_part != BI_NONE);
	m_script_bone_part_mask = CStalkerAnimationPair::all_bone_parts ^ (1 << bone_part);
#endif

	assign_bone_callbacks();

#ifdef DEBUG
	global().set_dbg_info		(*object().cName(),"Global");
	head().set_dbg_info			(*object().cName(),"Head  ");
	torso().set_dbg_info		(*object().cName(),"Torso ");
	legs().set_dbg_info			(*object().cName(),"Legs  ");
	script().set_dbg_info		(*object().cName(),"Script");
#endif

	m_global.reset();
	m_head.reset();
	m_torso.reset();
	m_legs.reset();
	m_script.reset();
};

void CStalkerAnimationManager::play_fx(float power_factor, int fx_index)
{
	VERIFY(fx_index >= 0);
	VERIFY(fx_index < (int)m_data_storage->m_part_animations.A[object().movement().body_state()].m_global.A[0].A.size())
	;
#ifdef DEBUG
	if (psAI_Flags.is(aiAnimation)) {
		LPCSTR					name = m_skeleton_animated->LL_MotionDefName_dbg(m_data_storage->m_part_animations.A[object().movement().body_state()].m_global.A[0].A[fx_index]).first;
		Msg						("%6d [%s][%s][%s][%f]",Device.dwTimeGlobal,*object().cName(),"FX",name,power_factor);
	}
#endif
	m_skeleton_animated->PlayFX(
		m_data_storage->m_part_animations.A[object().movement().body_state()].m_global.A[0].A[fx_index], power_factor);
}



void CStalkerAnimationManager::export_network_layers(CoopStalkerLayers& state)
{
    if (!object().g_Alive()) return;
    state.ready=1 | (object().animation_movement_controlled() ? 2 : 0);
    if (PIItem held=object().inventory().ActiveItem()) {
        state.held_id=held->object().ID();
        if (CHudItem* hud=held->cast_hud_item()) state.held_state=hud->GetState();
        // ready bit 2: the held weapon hangs on the back (same test as CObjectHandler::weapon_bones).
        if (object().CObjectHandler::planner().m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped)) state.ready|=4;
    }
    CStalkerAnimationPair* pairs[5]={&global(),&head(),&torso(),&legs(),&script()};
    for(u32 i=0;i<5;++i)
    {
        state.motion[i]=pairs[i]->animation().val;
        CBlend* b=pairs[i]->blend();
        if (pairs[i]->animation().valid() && b) { state.phase[i]=b->timeCurrent; state.speed[i]=b->playing ? b->speed : 0.f; state.amount[i]=b->blendAmount; state.duration[i]=b->timeTotal;
            if (!m_skeleton_animated->LL_GetMotionDef(pairs[i]->animation())->StopAtEnd()) state.looping|=u8(1u<<i); }
    }
    const Fmatrix* rotations[4]={&object().XFORM(),&object().sight().current_head_rotation(),
        &object().sight().current_shoulder_rotation(),&object().sight().current_spine_rotation()};
    for(u32 i=0;i<4;++i)
    {
        Fquaternion q; q.set(*rotations[i]); q.normalize();
        if (i && !object().sight().enabled()) q.set(1.f,0.f,0.f,0.f);
        if (i && m_head_params.m_blend && *m_head_params.m_blend)
        {
            const CBlend* b=*m_head_params.m_blend;
            float factor=b->timeTotal>EPS ? b->timeCurrent/b->timeTotal : 1.f;
            clamp(factor,0.f,1.f); if (!m_head_params.m_forward) factor=1.f-factor;
            Fquaternion identity,result; identity.set(1.f,0.f,0.f,0.f);
            result.slerp(identity,q,factor); result.normalize(); q=result;
        }
        state.rotation[i][0]=q.x; state.rotation[i][1]=q.y; state.rotation[i][2]=q.z; state.rotation[i][3]=q.w;
    }
    if (strstr(Core.Params,"-coop_npc_motion_probe"))
    {
        static xr_map<u16,u32> reports; u32& last=reports[object().ID()];
        if (Device.dwTimeGlobal-last>=250) {
            last=Device.dwTimeGlobal;
            Msg("[COOP_HELD] side=server owner=%u expected=%u actual=%u count=%u state=%u",object().ID(),state.held_id,state.held_id,u32(object().inventory().m_all.size()),state.held_state);
            Msg("[COOP_LAYERS] side=server id=%u time=%u ids=%u,%u,%u,%u,%u phases=%f,%f,%f,%f,%f root=%f,%f,%f,%f",
                object().ID(),Level().timeServer(),state.motion[0],state.motion[1],state.motion[2],state.motion[3],state.motion[4],
                state.phase[0],state.phase[1],state.phase[2],state.phase[3],state.phase[4],
                state.rotation[0][0],state.rotation[0][1],state.rotation[0][2],state.rotation[0][3]);
        }
    }
}

static void coop_network_root(CBoneInstance* bone) { bone->mTransform.identity(); }

void CStalkerAnimationManager::clear_network_root()
{
    IKinematics* k=smart_cast<IKinematics*>(m_visual);
    if (!k) return;
    CBoneInstance& root=k->LL_GetBoneInstance(k->LL_GetBoneRoot());
    if (root.callback()==coop_network_root) root.reset_callback();
}

void CStalkerAnimationManager::apply_network_layers(const CoopStalkerLayers& state)
{
    if (!state.ready || !object().g_Alive()) return;
    // Offscreen bones need not be evaluated, but replica clocks must advance.
    // UpdateTracks guards the current frame/time, so rendering cannot tick twice.
    m_skeleton_animated->UpdateTracks();
    PIItem previous=object().inventory().ActiveItem();
    object().inventory().SetNetworkActiveItem(state.held_id);
    PIItem held=object().inventory().ActiveItem();
    if (previous!=held && previous) object().attach(previous);
    object().m_coop_weapon_strapped=(state.ready & 4)!=0; // read by CObjectHandler::weapon_bones on the replica
    if (held) {
        object().detach(held);
        if (CHudItem* hud=held->cast_hud_item())
            if (hud->GetState()!=state.held_state) { hud->SetState(state.held_state); hud->SetNextState(state.held_state); }
    }
    if (strstr(Core.Params,"-coop_npc_motion_probe")) {
        static xr_map<u16,u32> reports; u32& last=reports[object().ID()];
        if (Device.dwTimeGlobal-last>=1000) {
            last=Device.dwTimeGlobal;
            Msg("[COOP_HELD] side=client owner=%u expected=%u actual=%u count=%u state=%u",object().ID(),state.held_id,held?held->object().ID():u16(-1),u32(object().inventory().m_all.size()),state.held_state);
        }
    }
    if (object().animation_movement()) object().destroy_anim_mov_ctrl();
    IKinematics* k=smart_cast<IKinematics*>(m_visual);
    CBoneInstance& root=k->LL_GetBoneInstance(k->LL_GetBoneRoot());
    if (state.ready & 2) root.set_callback(bctCustom,coop_network_root,this,TRUE);
    else clear_network_root();
    Fvector position=object().Position();
    for(u32 i=0;i<4;++i)
    {
        Fquaternion q; q.set(state.rotation[i][3],state.rotation[i][0],state.rotation[i][1],state.rotation[i][2]);
        q.normalize();
        if (i==0) { object().XFORM().rotation(q); object().XFORM().translate_over(position); }
        else m_network_rotations[i-1].rotation(q);
    }
    // No local Lua completion, AI selection, or animation-driven locomotion.
    // Server-selected script/global clips retain the same body partition mask.
    CStalkerAnimationPair* pairs[5]={&global(),&head(),&torso(),&legs(),&script()};
    for(u32 i=0;i<5;++i)
    {
        MotionID id; id.val=state.motion[i];
        if (!id.valid()) { pairs[i]->reset(); continue; }
        if (id.slot>=m_skeleton_animated->LL_MotionsSlotCount())
        { Msg("[COOP_LAYERS] INVALID_MOTION slot=%u index=%u",id.slot,id.idx); return; }
        shared_motions motions=m_skeleton_animated->LL_MotionsSlot(id.slot);
        if (id.idx>=motions.motion_defs()->size() || !_valid(state.phase[i]) || !_valid(state.speed[i]) || !_valid(state.amount[i]))
        { Msg("[COOP_LAYERS] INVALID_MOTION slot=%u index=%u",id.slot,id.idx); return; }
        CBlend* previous=pairs[i]->blend();
        if (previous && (previous->blend_state()==CBlend::eFREE_SLOT || previous->motionID!=pairs[i]->animation()))
            pairs[i]->reset();
        const bool restart=!pairs[i]->actual() || pairs[i]->animation()!=id;
        pairs[i]->animation(id);
        pairs[i]->play(m_skeleton_animated,0,false,false,false,
            (i==0 || i==4) ? m_script_bone_part_mask : CStalkerAnimationPair::all_bone_parts,i!=4);
        CBlend* b=pairs[i]->blend();
        if (b)
        {
            state.sync_blend(i,b,restart);
        }
        // Global/script pairs have one blend per partition, not just blend().
        if (i==0 || i==4)
            for(u16 part=0;part<MAX_PARTS;++part)
                if (m_script_bone_part_mask & (1u<<part))
                    for(u32 n=0;n<m_skeleton_animated->LL_PartBlendsCount(part);++n)
                    {
                        CBlend* b=m_skeleton_animated->LL_PartBlend(part,n);
                        if (b && b->motionID==id && b!=pairs[i]->blend()) state.sync_blend(i,b,restart);
                    }
    }
    assign_bone_callbacks();
    m_head_params.m_rotation=&m_network_rotations[0];
    m_shoulder_params.m_rotation=&m_network_rotations[1];
    m_spine_params.m_rotation=&m_network_rotations[2];
    smart_cast<IKinematics*>(m_visual)->CalculateBones_Invalidate();
    if (strstr(Core.Params,"-coop_npc_motion_probe"))
    {
        static xr_map<u16,u32> reports; u32& last=reports[object().ID()];
        if (Device.dwTimeGlobal-last>=250) {
            last=Device.dwTimeGlobal;
            Fquaternion q; q.set(object().XFORM());
            Msg("[COOP_LAYERS] side=client id=%u time=%u ids=%u,%u,%u,%u,%u phases=%f,%f,%f,%f,%f root=%f,%f,%f,%f",
                object().ID(),Level().timeServer(),global().animation().val,head().animation().val,torso().animation().val,legs().animation().val,script().animation().val,
                global().blend()?global().blend()->timeCurrent:0.f,head().blend()?head().blend()->timeCurrent:0.f,
                torso().blend()?torso().blend()->timeCurrent:0.f,legs().blend()?legs().blend()->timeCurrent:0.f,script().blend()?script().blend()->timeCurrent:0.f,
                q.x,q.y,q.z,q.w);
        }
    }
}

bool CStalkerAnimationManager::standing() const
{
	CAI_Stalker& obj = object();
	stalker_movement_manager_smart_cover& movement = obj.movement();

	if (movement.speed(obj.character_physics_support()->movement()) < EPS_L)
		return (true);

	if (eMovementTypeStand == movement.movement_type())
		return (true);

	return (false);
}
