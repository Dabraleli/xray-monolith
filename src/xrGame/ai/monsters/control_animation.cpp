#include "stdafx.h"
#include "control_animation.h"
#include "BaseMonster/base_monster.h"
#include "control_manager.h"
#include "profiler.h"
#include "../../level.h"

//#ifdef _DEBUG
//#include "control_animation_base.h"
//#endif



static void report_monster_layers(CBaseMonster* object, const CoopStalkerLayers& state)
{
    if (!strstr(Core.Params,"-coop_npc_motion_probe")) return;
    static xr_map<u16,u32> reports; u32& last=reports[object->ID()];
    // Record every exported server snapshot: fast turns are not linear over 250 ms.
    if (object->Remote() && Device.dwTimeGlobal-last<250) return;
    last=Device.dwTimeGlobal;
    Fquaternion q; q.set(object->XFORM());
    Msg("[COOP_MONSTER_LAYERS] side=%s id=%u time=%u ids=%u,%u,%u,%u,%u phases=%f,%f,%f,%f,%f root=%f,%f,%f,%f",
        object->Remote()?"client":"server",object->ID(),Level().timeServer(),
        state.motion[0],state.motion[1],state.motion[2],state.motion[3],state.motion[4],
        state.phase[0],state.phase[1],state.phase[2],state.phase[3],state.phase[4],q.x,q.y,q.z,q.w);
}

void CControlAnimation::export_network_layers(CoopStalkerLayers& state)
{
    if (!m_object->g_Alive()) return;
    state.ready=1;
    SAnimationPart* parts[3]={&m_data.global,&m_data.legs,&m_data.torso};
    for(u32 i=0;i<3;++i)
    {
        CBlend* b=parts[i]->blend;
        if (!b || !parts[i]->get_motion().valid()) continue;
        state.motion[i]=parts[i]->get_motion().val;
        state.phase[i]=b->timeCurrent; state.speed[i]=b->playing ? b->speed : 0.f; state.amount[i]=b->blendAmount;
        state.duration[i]=b->timeTotal;
        if (!m_skeleton_animated->LL_GetMotionDef(parts[i]->get_motion())->StopAtEnd()) state.looping|=u8(1u<<i);
    }
    Fquaternion q; q.set(m_object->XFORM()); q.normalize();
    state.rotation[0][0]=q.x; state.rotation[0][1]=q.y; state.rotation[0][2]=q.z; state.rotation[0][3]=q.w;
    report_monster_layers(m_object,state);
}

void CControlAnimation::apply_network_layers(const CoopStalkerLayers& state)
{
    if (!state.ready || !m_object->g_Alive()) return;
    // Offscreen bones need not be evaluated, but replica clocks must advance.
    // UpdateTracks guards the current frame/time, so rendering cannot tick twice.
    m_skeleton_animated->UpdateTracks();
    Fvector position=m_object->Position();
    Fquaternion q; q.set(state.rotation[0][3],state.rotation[0][0],state.rotation[0][1],state.rotation[0][2]);
    q.normalize();
    m_object->XFORM().rotation(q); m_object->XFORM().translate_over(position);
    SAnimationPart* parts[3]={&m_data.global,&m_data.legs,&m_data.torso};
    CoopStalkerLayers applied=state;
    for(u32 i=0;i<3;++i)
    {
        MotionID id; id.val=state.motion[i];
        if (!id.valid()) { parts[i]->init(); continue; }
        if (id.slot>=m_skeleton_animated->LL_MotionsSlotCount())
        { Msg("[COOP_MONSTER_LAYERS] INVALID_MOTION"); return; }
        shared_motions motions=m_skeleton_animated->LL_MotionsSlot(id.slot);
        if(id.idx>=motions.motion_defs()->size() || !_valid(state.phase[i]) || !_valid(state.speed[i]) || !_valid(state.amount[i]))
        { Msg("[COOP_MONSTER_LAYERS] INVALID_MOTION"); return; }
        CBlend* b=parts[i]->blend;
        const bool restart=!b || parts[i]->get_motion()!=id || b->motionID!=id || b->blend_state()==CBlend::eFREE_SLOT;
        if (restart)
        {
            u16 part=m_skeleton_animated->LL_GetMotionDef(id)->bone_or_part;
            if(part==u16(-1)) part=m_skeleton_animated->LL_PartID("default");
            b=m_skeleton_animated->LL_PlayCycle(part,id,TRUE,0,0);
            parts[i]->set_motion(id); parts[i]->blend=b; parts[i]->actual=true;
            if (b && i!=2) m_object->CStepManager::on_animation_start(id,b);
        }
        if(b)
        {
            state.sync_blend(i,b,restart);
            applied.phase[i]=b->timeCurrent; applied.motion[i]=b->motionID.val;
        }
    }
    m_skeleton_animated->dcast_PKinematics()->CalculateBones_Invalidate();
    report_monster_layers(m_object,applied);
}

void SAnimationPart::set_motion(MotionID const& m)
{
	VERIFY(m.valid());
	motion = m;
}

void CControlAnimation::reinit()
{
	inherited::reinit();

	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());

	m_anim_events.clear();

	m_global_animation_end = false;
	m_legs_animation_end = false;
	m_torso_animation_end = false;

	m_freeze = false;
}

void CControlAnimation::reset_data()
{
	m_data.global.init();
	m_data.legs.init();
	m_data.torso.init();
	m_data.set_speed(-1.f);
}

void CControlAnimation::update_frame()
{
	if (m_freeze || (IsGameTypeCoop() && m_object->Remote())) return;

	// move to schedule update
	START_PROFILE("BaseMonster/Animation/Update Tracks")
		;
		m_skeleton_animated->UpdateTracks();
	STOP_PROFILE;

	START_PROFILE("BaseMonster/Animation/Check callbacks")
		;
		check_callbacks();
	STOP_PROFILE;

	START_PROFILE("BaseMonster/Animation/Play")
		;
		play();
	STOP_PROFILE;

	START_PROFILE("BaseMonster/Animation/Check Events")
		;
		check_events(m_data.global);
		check_events(m_data.torso);
		check_events(m_data.legs);
	STOP_PROFILE;
}

static void global_animation_end_callback(CBlend* B)
{
	CControlAnimation* controller = (CControlAnimation *)B->CallbackParam;
	controller->m_global_animation_end = true;
}

static void legs_animation_end_callback(CBlend* B)
{
	CControlAnimation* controller = (CControlAnimation *)B->CallbackParam;
	controller->m_legs_animation_end = true;
}

static void torso_animation_end_callback(CBlend* B)
{
	CControlAnimation* controller = (CControlAnimation *)B->CallbackParam;
	controller->m_torso_animation_end = true;
}

void CControlAnimation::play()
{
	if (!m_data.global.actual)
	{
		play_part(m_data.global, global_animation_end_callback);
		if (m_data.global.blend) m_saved_global_speed = m_data.global.blend->speed;
	}

	if (!m_data.legs.actual)
		play_part(m_data.legs, legs_animation_end_callback);
	if (!m_data.torso.actual)
		play_part(m_data.torso, torso_animation_end_callback);

	// speed only for global
	if (m_data.global.blend)
	{
		if (m_data.get_speed() > 0)
		{
			m_data.global.blend->speed = m_data.get_speed(); // TODO: make factor
		}
		else
		{
			m_data.global.blend->speed = m_saved_global_speed;
		}
	}
}

void CControlAnimation::play_part(SAnimationPart& part, PlayCallback callback)
{
	VERIFY(part.get_motion().valid());

	u16 bone_or_part = m_skeleton_animated->LL_GetMotionDef(part.get_motion())->bone_or_part;
	if (bone_or_part == u16(-1)) bone_or_part = m_skeleton_animated->LL_PartID("default");

	// initialize synchronization of prev and current animation
	float pos = -1.f;
	if (part.blend && !part.blend->stop_at_end)
		pos = fmod(part.blend->timeCurrent, part.blend->timeTotal) / part.blend->timeTotal;
#ifdef DEBUG
	//IKinematicsAnimated * K = m_object->Visual()->dcast_PKinematicsAnimated();
	//Msg				("%6d Playing animation : %s , %s , Object %s",Device.dwTimeGlobal, K->LL_MotionDefName_dbg(part.motion).first,K->LL_MotionDefName_dbg(part.motion).second, *(m_object->cName()));
#endif

	part.blend = m_skeleton_animated->LL_PlayCycle(bone_or_part, part.get_motion(), TRUE, callback, this);


	///////////////////////////////////////////////////////////////////////////////
	//#ifdef _DEBUG	
	//	Msg("Monster[%s] Time[%u] Anim[%s]",*(m_object->cName()), Device.dwTimeGlobal,*(m_object->anim().GetAnimTranslation(part.motion)));
	//#endif
	///////////////////////////////////////////////////////////////////////////////

	// synchronize prev and current animations
	if ((pos > 0) && part.blend && !part.blend->stop_at_end)
	{
		part.blend->timeCurrent = part.blend->timeTotal * pos;
	}

	part.time_started = Device.dwTimeGlobal;
	part.actual = true;

	m_man->notify(ControlCom::eventAnimationStart, 0);

	if ((part.get_motion() != m_data.torso.get_motion()) && part.blend)
		m_object->CStepManager::on_animation_start(part.get_motion(), part.blend);


	ANIMATION_EVENT_MAP_IT it = m_anim_events.find(part.get_motion());
	if (it != m_anim_events.end())
	{
		for (ANIMATION_EVENT_VEC_IT event_it = it->second.begin(); event_it != it->second.end(); ++event_it)
		{
			event_it->handled = false;
		}
	}
}


void CControlAnimation::add_anim_event(MotionID motion, float time_perc, u32 id)
{
	// if there is already event with exact timing - return
	ANIMATION_EVENT_MAP_IT it = m_anim_events.find(motion);
	if (it != m_anim_events.end())
	{
		ANIMATION_EVENT_VEC& anim_vec = it->second;

		for (ANIMATION_EVENT_VEC_IT I = anim_vec.begin(); I != anim_vec.end(); ++I)
		{
			if (fsimilar(I->time_perc, time_perc)) return;
		}
	}

	SAnimationEvent event;
	event.time_perc = time_perc;
	event.event_id = id;

	m_anim_events[motion].push_back(event);
}

void CControlAnimation::check_events(SAnimationPart& part)
{
	if (part.get_motion().valid() && part.actual && part.blend)
	{
		ANIMATION_EVENT_MAP_IT it = m_anim_events.find(part.get_motion());
		if (it != m_anim_events.end())
		{
			float cur_perc = float(Device.dwTimeGlobal - part.time_started) / ((part.blend->timeTotal / part
			                                                                                            .blend->speed) *
				1000);

			for (ANIMATION_EVENT_VEC_IT event_it = it->second.begin(); event_it != it->second.end(); ++event_it)
			{
				SAnimationEvent& event = *event_it;
				if (!event.handled && (event.time_perc < cur_perc))
				{
					event.handled = true;

					// gen event
					SAnimationSignalEventData anim_event(part.get_motion(), event.time_perc, event.event_id);
					m_man->notify(ControlCom::eventAnimationSignal, &anim_event);
				}
			}
		}
	}
}

void CControlAnimation::check_callbacks()
{
	if (m_global_animation_end)
	{
		m_man->notify(ControlCom::eventAnimationEnd, 0);
		m_global_animation_end = false;
	}

	if (m_legs_animation_end)
	{
		m_man->notify(ControlCom::eventLegsAnimationEnd, 0);
		m_legs_animation_end = false;
	}

	if (m_torso_animation_end)
	{
		m_man->notify(ControlCom::eventTorsoAnimationEnd, 0);
		m_torso_animation_end = false;
	}
}

void CControlAnimation::restart(SAnimationPart& part, PlayCallback callback)
{
	VERIFY(part.get_motion().valid());
	VERIFY(part.blend);

	u16 bone_or_part = m_skeleton_animated->LL_GetMotionDef(part.get_motion())->bone_or_part;
	if (bone_or_part == u16(-1)) bone_or_part = m_skeleton_animated->LL_PartID("default");

	//save 
	float time_saved = part.blend->timeCurrent;

	// start
	part.blend = m_skeleton_animated->LL_PlayCycle(bone_or_part, part.get_motion(), TRUE, callback, this);

	// restore
	part.blend->timeCurrent = time_saved;
}

void CControlAnimation::restart()
{
	m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_object->Visual());

	if (m_data.global.blend) restart(m_data.global, global_animation_end_callback);
	if (m_data.legs.blend) restart(m_data.legs, legs_animation_end_callback);
	if (m_data.torso.blend) restart(m_data.torso, torso_animation_end_callback);
}

void CControlAnimation::freeze()
{
	if (m_freeze) return;
	m_freeze = true;

	if (m_data.global.blend)
	{
		m_saved_global_speed = m_data.global.blend->speed;
		m_data.global.blend->speed = 0.f;
	}
	if (m_data.legs.blend)
	{
		m_saved_legs_speed = m_data.legs.blend->speed;
		m_data.legs.blend->speed = 0.f;
	}
	if (m_data.torso.blend)
	{
		m_saved_torso_speed = m_data.torso.blend->speed;
		m_data.torso.blend->speed = 0.f;
	}
}

void CControlAnimation::unfreeze()
{
	if (!m_freeze) return;
	m_freeze = false;

	if (m_data.global.blend)
	{
		m_data.global.blend->speed = m_saved_global_speed;
	}
	if (m_data.legs.blend)
	{
		m_data.legs.blend->speed = m_saved_legs_speed;
	}
	if (m_data.torso.blend)
	{
		m_data.torso.blend->speed = m_saved_torso_speed;
	}
}
