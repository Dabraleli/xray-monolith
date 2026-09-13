////////////////////////////////////////////////////////////////////////////
//	Module 		: base_monster_anim.cpp
//	Created 	: 22.05.2003
//  Modified 	: 23.09.2003
//	Author		: Serge Zhem
//	Description : Animations for monsters of biting class 
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "base_monster.h"
#include "../../../../Include/xrRender/KinematicsAnimated.h"
#include "../../../sound_player.h"
#include "../../../ai_monster_space.h"
#include "../control_animation_base.h"
#include "../control_animation.h"

// Установка анимации
void CBaseMonster::SelectAnimation(const Fvector&/**_view/**/, const Fvector&/**_move/**/, float /**speed/**/)
{
    if (IsGameTypeCoop() && Remote()) control().animation().apply_network_layers(NET_Last.coop_layers);
    else control().animation().update_frame();
}
