#include "pch_script.h"
#include "trader_animation.h"
#include "ai_trader.h"
#include "../../script_callback_ex.h"
#include "../../game_object_space.h"
#include "../../game_sv_coop.h"


/////////////////////////////////////////////////////////////////////////////////////////
// Startup
/////////////////////////////////////////////////////////////////////////////////////////

void CTraderAnimation::reinit()
{
	m_motion_head.invalidate();
	m_motion_global.invalidate();
	m_sound = 0;
	m_external_sound = 0;
	m_coop_relayed_sound = false;

	m_anim_global = 0;
	m_anim_head = 0;
}


/////////////////////////////////////////////////////////////////////////////////////////
// Animation Callbacks
/////////////////////////////////////////////////////////////////////////////////////////
void CTraderAnimation::global_callback(CBlend* B)
{
	CTraderAnimation* trader = (CTraderAnimation*)B->CallbackParam;
	trader->m_motion_global.invalidate();
}

void CTraderAnimation::head_callback(CBlend* B)
{
	CTraderAnimation* trader = (CTraderAnimation*)B->CallbackParam;
	trader->m_motion_head.invalidate();
}

/////////////////////////////////////////////////////////////////////////////////////////
// Animation management
/////////////////////////////////////////////////////////////////////////////////////////
void CTraderAnimation::set_animation(LPCSTR anim)
{
	m_anim_global = anim;
	m_coop_global = anim;
	++m_coop_global_serial;

	IKinematicsAnimated* kinematics_animated = smart_cast<IKinematicsAnimated*>(m_trader->Visual());
	m_motion_global = kinematics_animated->ID_Cycle(m_anim_global);
	kinematics_animated->PlayCycle(m_motion_global,TRUE, global_callback, this);
}

void CTraderAnimation::set_head_animation(LPCSTR anim)
{
	m_anim_head = anim;
	m_coop_head = anim;
	++m_coop_head_serial;

	// назначить анимацию головы
	IKinematicsAnimated* kinematics_animated = smart_cast<IKinematicsAnimated*>(m_trader->Visual());
	m_motion_head = kinematics_animated->ID_Cycle(m_anim_head);
	kinematics_animated->PlayCycle(m_motion_head,TRUE, head_callback, this);
}

void CTraderAnimation::coop_apply(u16 global_serial, LPCSTR global, u16 head_serial, LPCSTR head)
{
	IKinematicsAnimated* kinematics_animated = smart_cast<IKinematicsAnimated*>(m_trader->Visual());
	if (!kinematics_animated) return;
	if (global_serial != m_coop_global_serial && global && *global)
	{
		m_coop_global_serial = global_serial;
		m_coop_global = global;
		m_anim_global = m_coop_global.c_str();
		m_motion_global = kinematics_animated->ID_Cycle_Safe(m_anim_global);
		if (m_motion_global.valid()) kinematics_animated->PlayCycle(m_motion_global, TRUE, global_callback, this);
		else Msg("! [COOP_TRADER] unknown global animation %s for %s", global, m_trader->cName().c_str());
	}
	if (head_serial != m_coop_head_serial && head && *head)
	{
		m_coop_head_serial = head_serial;
		m_coop_head = head;
		m_anim_head = m_coop_head.c_str();
		m_motion_head = kinematics_animated->ID_Cycle_Safe(m_anim_head);
		if (m_motion_head.valid()) kinematics_animated->PlayCycle(m_motion_head, TRUE, head_callback, this);
		else Msg("! [COOP_TRADER] unknown head animation %s for %s", head, m_trader->cName().c_str());
	}
}

//////////////////////////////////////////////////////////////////////////
// Sound management
//////////////////////////////////////////////////////////////////////////
void CTraderAnimation::set_sound(LPCSTR sound, LPCSTR anim)
{
	if (m_sound) remove_sound();

	set_head_animation(anim);

	m_sound = xr_new<ref_sound>();
	m_sound->create(sound, st_Effect, SOUND_TYPE_WORLD);
	m_sound->play_at_pos(m_trader, m_trader->Position());
	m_sound_started = Device.dwTimeGlobal;
	game_sv_Coop::RelayTraderSound(m_trader, sound);
}

void CTraderAnimation::remove_sound(bool notify_clients)
{
	VERIFY(m_sound);
	if (IsGameTypeCoop() && strstr(Core.Params, "-coop_damage_probe"))
		Msg("[COOP_TRADER_VOICE_END] id=%u remote=%u relayed=%u elapsed_ms=%u length_ms=%u notify=%u", m_trader->ID(), m_trader->Remote() ? 1 : 0, m_coop_relayed_sound ? 1 : 0, Device.dwTimeGlobal - m_sound_started, u32(m_sound->get_length_sec() * 1000.f), notify_clients ? 1 : 0);
	if (notify_clients) game_sv_Coop::RelayTraderSound(m_trader, NULL);

	if (m_sound->_feedback())
		m_sound->stop();

	m_sound->destroy();
	xr_delete(m_sound);
	m_coop_relayed_sound = false;
}

//////////////////////////////////////////////////////////////////////////
// Update 
//////////////////////////////////////////////////////////////////////////
void CTraderAnimation::update_frame()
{
	if (m_sound)
	{
		if (m_sound->_feedback())
			m_sound->set_position(m_trader->Position());
		else
		{
			m_trader->callback(GameObject::eTraderSoundEnd)();
			// Each listener finishes its own emitter. The server may have no audible target;
			// its natural completion must not truncate the remote listener's phrase.
			remove_sound(false);
		}
	}

	if (!m_motion_global)
	{
		m_trader->callback(GameObject::eTraderGlobalAnimationRequest)();
		if (m_anim_global) m_motion_head.invalidate();
	}

	// íàçíà÷èòü àíèìàöèþ ãîëîâû
	if (!m_motion_head)
	{
		if (m_sound && m_sound->_feedback())
		{
			m_trader->callback(GameObject::eTraderHeadAnimationRequest)();
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// External sound support 
//////////////////////////////////////////////////////////////////////////
void CTraderAnimation::external_sound_start(LPCSTR phrase)
{
	if (m_sound) remove_sound();

	m_sound = xr_new<ref_sound>();
	m_sound->create(phrase, st_Effect, SOUND_TYPE_WORLD);
	m_sound->play_at_pos(m_trader, m_trader->Position());
	m_sound_started = Device.dwTimeGlobal;
	m_coop_relayed_sound = false;
	if (IsGameTypeCoop() && strstr(Core.Params, "-coop_damage_probe"))
		Msg("[COOP_TRADER_VOICE] id=%u remote=%u path=%s playing=%u pos=%f,%f,%f", m_trader->ID(), m_trader->Remote() ? 1 : 0, phrase, m_sound->_feedback() ? 1 : 0, VPUSH(m_trader->Position()));
	game_sv_Coop::RelayTraderSound(m_trader, phrase);

	m_motion_head.invalidate();
}

void CTraderAnimation::coop_sound(LPCSTR path)
{
    if (path && *path)
    {
        // The talking player's UI owns an external phrase until it finishes or stops.
        if (m_sound && !m_coop_relayed_sound && m_sound->_feedback()) return;
        external_sound_start(path);
        m_coop_relayed_sound = true;
    }
    else if (m_coop_relayed_sound && m_sound)
        remove_sound(false); // explicit stop from the server owns this relayed slot
}

void CTraderAnimation::external_sound_stop()
{
	// UITalkWnd calls this even for an unvoiced line, and when closing the window.
	// That UI owns local dialogue audio, not the server's greeting/farewell.
	if (IsGameTypeCoop() && m_trader->Remote() && m_coop_relayed_sound)
	{
		if (strstr(Core.Params, "-coop_damage_probe"))
			Msg("[COOP_TRADER_UI_STOP_IGNORED] id=%u elapsed_ms=%u", m_trader->ID(), Device.dwTimeGlobal - m_sound_started);
		return;
	}
	if (m_sound) remove_sound();
}

//////////////////////////////////////////////////////////////////////////
