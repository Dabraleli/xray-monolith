#pragma once

#include "../../../Include/xrRender/KinematicsAnimated.h"

class CAI_Trader;

namespace MonsterSpace
{
	enum EMonsterHeadAnimType;
};

class CTraderAnimation
{
	CAI_Trader* m_trader;

	LPCSTR m_anim_global;
	LPCSTR m_anim_head;

	MotionID m_motion_head;
	MotionID m_motion_global;

	ref_sound* m_sound;

	bool m_external_sound;

	// Coop: what the server Lua last asked for, replicated to clients (CAI_Trader::net_Export).
	// Own copies: the LPCSTR fields above point into Lua strings.
	shared_str m_coop_global;
	shared_str m_coop_head;
	u16 m_coop_global_serial;
	u16 m_coop_head_serial;

public:
	CTraderAnimation(CAI_Trader* trader) : m_trader(trader), m_coop_global_serial(0), m_coop_head_serial(0)
	{
	}

	void reinit();

	void set_animation(LPCSTR anim);
	void set_head_animation(LPCSTR anim);
	void set_sound(LPCSTR sound, LPCSTR head_anim);

	const shared_str& coop_global() const { return m_coop_global; }
	const shared_str& coop_head() const { return m_coop_head; }
	u16 coop_global_serial() const { return m_coop_global_serial; }
	u16 coop_head_serial() const { return m_coop_head_serial; }
	// Client replica: play what the server reported (serials change on every set_* call there).
	void coop_apply(u16 global_serial, LPCSTR global, u16 head_serial, LPCSTR head);

	// Callbacks
	static void global_callback(CBlend* B);
	static void head_callback(CBlend* B);

	void update_frame();

	void external_sound_start(LPCSTR phrase);
	void external_sound_stop();

private:
	void remove_sound();
};
