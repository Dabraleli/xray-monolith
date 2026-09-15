#include "stdafx.h"
#include "actor.h"
#include "customdetector.h"
#include "weapon.h"
#include "artefact.h"
#include "scope.h"
#include "silencer.h"
#include "grenadelauncher.h"
#include "inventory.h"
#include "level.h"
#include "xr_level_controller.h"
#include "FoodItem.h"
#include "ActorCondition.h"
#include "UIGameCustom.h"
#include "ui/UIActorMenu.h"
#include "Grenade.h"

#include "CameraLook.h"
#include "CameraFirstEye.h"
#include "holder_custom.h"
//.#include "ui/uiinventoryWnd.h"
#include "game_base_space.h"
#ifdef DEBUG
#include "PHDebug.h"
#endif
#include <luabind/luabind.hpp>
#include "script_game_object.h"
#include "game_sv_coop.h"
#include "UIGameSP.h"

void CActor::OnEvent(NET_Packet& P, u16 type)
{
	inherited::OnEvent(P, type);
	CInventoryOwner::OnEvent(P, type);

	u16 id;
	switch (type)
	{
	case GE_TRADE_BUY:
	case GE_OWNERSHIP_TAKE:
		{
			P.r_u16(id);
			CObject* Obj = Level().Objects.net_Find(id);

			//			R_ASSERT2( Obj, make_string("GE_OWNERSHIP_TAKE: Object not found. object_id = [%d]", id).c_str() );
			VERIFY2(Obj, make_string("GE_OWNERSHIP_TAKE: Object not found. object_id = [%d]", id).c_str());
			if (!Obj)
			{
				Msg("! GE_OWNERSHIP_TAKE: Object not found. object_id = [%d]", id);
				break;
			}

			CGameObject* _GO = smart_cast<CGameObject*>(Obj);
			if (!IsGameTypeSingle() && !g_Alive())
			{
				Msg("! WARNING: dead player [%d][%s] can't take items [%d][%s]",
				    ID(), Name(), _GO->ID(), _GO->cNameSect().c_str());
				break;
			}

			if (inventory().CanTakeItem(smart_cast<CInventoryItem*>(_GO)))
			{
				Obj->H_SetParent(smart_cast<CObject*>(this));

#ifdef MP_LOGGING
				string64 act;
				xr_strcpy( act, (type == GE_TRADE_BUY)? "buys" : "takes" );
				Msg("--- Actor [%d][%s]  %s  [%d][%s]", ID(), Name(), act, _GO->ID(), _GO->cNameSect().c_str());
#endif // MP_LOGGING

				inventory().Take(_GO, false, true);

				SelectBestWeapon(Obj);
			}
			else
			{
				if (IsGameTypeSingle())
				{
					NET_Packet P;
					u_EventGen(P, GE_OWNERSHIP_REJECT, ID());
					P.w_u16(u16(Obj->ID()));
					u_EventSend(P);
				}
				else
				{
					Msg("! ERROR: Actor [%d][%s]  tries to drop on take [%d][%s]", ID(), Name(), _GO->ID(),
					    _GO->cNameSect().c_str());
				}
			}
		}
		break;
	case GE_TRADE_SELL:
	case GE_OWNERSHIP_REJECT:
		{
			P.r_u16(id);
			CObject* Obj = Level().Objects.net_Find(id);

			//			R_ASSERT2( Obj, make_string("GE_OWNERSHIP_REJECT: Object not found, id = %d", id).c_str() );
			VERIFY2(Obj, make_string("GE_OWNERSHIP_REJECT: Object not found, id = %d", id).c_str());
			if (!Obj)
			{
				Msg("! GE_OWNERSHIP_REJECT: Object not found, id = %d", id);
				break;
			}

			bool just_before_destroy = !P.r_eof() && P.r_u8();
			bool dont_create_shell = (type == GE_TRADE_SELL) || just_before_destroy;
			Obj->SetTmpPreDestroy(just_before_destroy);

			CGameObject* GO = smart_cast<CGameObject*>(Obj);

#ifdef MP_LOGGING
			string64 act;
			xr_strcpy( act, (type == GE_TRADE_SELL)? "sells" : "rejects" );
			Msg("--- Actor [%d][%s]  %s  [%d][%s]", ID(), Name(), act, GO->ID(), GO->cNameSect().c_str());
#endif // MP_LOGGING

			VERIFY(GO->H_Parent());
			if (!GO->H_Parent())
			{
				Msg("! ERROR: Actor [%d][%s] tries to reject item [%d][%s] that has no parent",
				    ID(), Name(), GO->ID(), GO->cNameSect().c_str());
				break;
			}

			VERIFY2(GO->H_Parent()->ID() == ID(),
			        make_string("actor [%d][%s] tries to drop not own object [%d][%s]",
				        ID(), Name(), GO->ID(), GO->cNameSect().c_str() ).c_str());

			if (GO->H_Parent()->ID() != ID())
			{
				CActor* real_parent = smart_cast<CActor*>(GO->H_Parent());
				Msg("! ERROR: Actor [%d][%s] tries to drop not own item [%d][%s], his parent is [%d][%s]",
				    ID(), Name(), GO->ID(), GO->cNameSect().c_str(), real_parent->ID(), real_parent->Name());
				break;
			}

			if (!Obj->getDestroy() && inventory().DropItem(GO, just_before_destroy, dont_create_shell))
			{
				//O->H_SetParent(0,just_before_destroy);//moved to DropItem
				//feel_touch_deny(O,2000);
				Level().m_feel_deny.feel_touch_deny(Obj, 1000);

				// [12.11.07] Alexander Maniluk: extended GE_OWNERSHIP_REJECT packet for drop item to selected position
				Fvector dropPosition;
				if (!P.r_eof())
				{
					P.r_vec3(dropPosition);
					GO->MoveTo(dropPosition);
					//Other variant :)
					/*NET_Packet MovePacket;
					MovePacket.w_begin(M_MOVE_ARTEFACTS);
					MovePacket.w_u8(1);
					MovePacket.w_u16(id);
					MovePacket.w_vec3(dropPosition);
					u_EventSend(MovePacket);*/
				}
			}

			if (!just_before_destroy)
				SelectBestWeapon(Obj);
		}
		break;
	case GE_INV_ACTION:
		{
			u16 cmd;
			P.r_u16(cmd);
			u32 flags;
			P.r_u32(flags);
			s32 ZoomRndSeed = P.r_s32();
			s32 ShotRndSeed = P.r_s32();
			if (!IsGameTypeSingle() && !g_Alive())
			{
				//				Msg("! WARNING: dead player tries to rize inventory action");
				break;
			}

			if (flags & CMD_START)
			{
				if (cmd == kWPN_ZOOM)
					SetZoomRndSeed(ZoomRndSeed);
				if (cmd == kWPN_FIRE)
					SetShotRndSeed(ShotRndSeed);
				IR_OnKeyboardPress(cmd);
			}
			else
				IR_OnKeyboardRelease(cmd);
		}
		break;
	case GEG_PLAYER_ITEM2SLOT:
	case GEG_PLAYER_ITEM2BELT:
	case GEG_PLAYER_ITEM2RUCK:
	case GEG_PLAYER_ITEM_EAT:
	case GEG_PLAYER_ACTIVATEARTEFACT:
		{
			P.r_u16(id);
			CObject* Obj = Level().Objects.net_Find(id);

			//			R_ASSERT2( Obj, make_string("GEG_PLAYER_ITEM_EAT(use): Object not found. object_id = [%d]", id).c_str() );
			VERIFY2(Obj, make_string("GEG_PLAYER_ITEM_EAT(use): Object not found. object_id = [%d]", id).c_str());
			if (!Obj)
			{
				//				Msg                 ( "! GEG_PLAYER_ITEM_EAT(use): Object not found. object_id = [%d]", id );
				break;
			}

			//			R_ASSERT2( !Obj->getDestroy(), make_string("GEG_PLAYER_ITEM_EAT(use): Object is destroying. object_id = [%d]", id).c_str() );
			VERIFY2(!Obj->getDestroy(),
			        make_string("GEG_PLAYER_ITEM_EAT(use): Object is destroying. object_id = [%d]", id).c_str());
			if (Obj->getDestroy())
			{
				//				Msg                                ( "! GEG_PLAYER_ITEM_EAT(use): Object is destroying. object_id = [%d]", id );
				break;
			}

			if (!IsGameTypeSingle() && !g_Alive())
			{
				Msg("! WARNING: dead player [%d][%s] can't use items [%d][%s]",
				    ID(), Name(), Obj->ID(), Obj->cNameSect().c_str());
				break;
			}

			if (type == GEG_PLAYER_ACTIVATEARTEFACT)
			{
				CArtefact* pArtefact = smart_cast<CArtefact*>(Obj);
				//			R_ASSERT2( pArtefact, make_string("GEG_PLAYER_ACTIVATEARTEFACT: Artefact not found. artefact_id = [%d]", id).c_str() );
				VERIFY2(pArtefact,
				        make_string("GEG_PLAYER_ACTIVATEARTEFACT: Artefact not found. artefact_id = [%d]", id).c_str());
				if (!pArtefact)
				{
					Msg("! GEG_PLAYER_ACTIVATEARTEFACT: Artefact not found. artefact_id = [%d]", id);
					break; //1
				}

				pArtefact->ActivateArtefact();
				break; //1
			}

			PIItem iitem = smart_cast<CInventoryItem*>(Obj);
			R_ASSERT(iitem);

			switch (type)
			{
			case GEG_PLAYER_ITEM2SLOT:
				{
					u16 slot_id = P.r_u16();
                    bool bDoNotActivate = P.r_u8() == 1; // conservatively preserve old behaviour unless very specifically (value == 1) requested otherwise
					inventory().Slot(slot_id, iitem, bDoNotActivate);
				}
				break; //2
			case GEG_PLAYER_ITEM2BELT:
				inventory().Belt(iitem);
				break; //2
			case GEG_PLAYER_ITEM2RUCK:
				inventory().Ruck(iitem);
				break; //2
			case GEG_PLAYER_ITEM_EAT:
				{
					// Coop server: the owning client already ran the per-player Lua rule (booster
					// stacking, required tools) against its own body. The server's Lua only knows the
					// world actor, so re-checking here would apply one player's boosters to everybody.
					const bool coop_body = IsGameTypeCoop() && OnServer() && this != Level().CurrentControlEntity();
					bool allowed = coop_body;
					::luabind::functor<bool> funct;
					if (!coop_body && iitem && ai().script_engine().functor("_G.CInventory__eat", funct))
					{
						CGameObject* GO = iitem->cast_game_object();
						allowed = GO && funct(GO->lua_game_object());
					}
					if (iitem && allowed)
						inventory().Eat(iitem);
				}
				break; //2
			} //switch
		}
		break; //1
	case GEG_PLAYER_ACTIVATE_SLOT:
		{
			u16 slot_id;
			P.r_u16(slot_id);

			inventory().Activate(slot_id);
		}
		break;

	case GEG_PLAYER_DISABLE_SPRINT:
		{
			s8 cmd = P.r_s8();
			m_block_sprint_counter = m_block_sprint_counter + cmd;
			//Msg("m_block_sprint_counter=%d", m_block_sprint_counter);
			if (m_block_sprint_counter > 0)
			{
				mstate_wishful &= ~mcSprint;
			}
			else
				m_block_sprint_counter = 0;
		}
		break;

	case GEG_PLAYER_WEAPON_HIDE_STATE:
		{
			u16 State = P.r_u16();
			BOOL Set = !!P.r_u8();
			inventory().SetSlotsBlocked(State, !!Set);
		}
		break;
	case GE_COOP_CONDITION:
		{
			// Coop: the server's condition for this body; the owning client's HUD shows it instead of
			// local guesses: bleeding, satiety and the active boosters (see CActor::shedule_Update).
			float bleeding, satiety;
			P.r_float_q8(bleeding, 0.f, 2.f);
			P.r_float_q8(satiety, 0.f, 1.f);
			CEntityCondition::BOOSTER_MAP boosters;
			for (u8 count = P.r_u8(); count > 0; --count)
			{
				SBooster B;
				B.m_type = (EBoostParams)P.r_u8();
				B.fBoostValue = P.r_float();
				B.fBoostTime = P.r_float();
				boosters[B.m_type] = B;
			}
			if (OnClient() && IsGameTypeCoop())
			{
				conditions().SetRemoteBleeding(bleeding);
				if (this == Level().CurrentControlEntity())
				{
					conditions().SetSatiety(satiety);
					conditions().SetRemoteBoosters(boosters);
				}
			}
		}
		break;
	case GEG_PLAYER_USE_BOOSTER:
		{
			// Coop client: the server body used a portioned item (CEatableItem::UseBy sends this in
			// every non-single game); only the replica's remaining uses follow, the effects come with
			// GE_COOP_CONDITION and the health update.
			const u16 item_id = P.r_u16();
			if (!(IsGameTypeCoop() && OnClient())) break;
			CEatableItem* eatable = smart_cast<CEatableItem*>(Level().Objects.net_Find(item_id));
			if (!eatable) break;
			const u8 uses = eatable->GetRemainingUses();
			if (uses != u8(-1) && uses > 0)
				eatable->SetRemainingUses(uses - 1);
			if (CurrentGameUI() && this == Level().CurrentControlEntity())
				CurrentGameUI()->GetActorMenu().RefreshCurrentItemCell();
		}
		break;
	case GE_COOP_HEALTH_CHANGE:
		{
			// Coop server: a health delta requested by the owning client's Lua (thirst/sleep penalties).
			const float delta = P.r_float();
			if (IsGameTypeCoop() && OnServer() && g_Alive() && this != Level().CurrentControlEntity())
				conditions().ChangeHealth(delta);
		}
		break;
	case GE_MONEY:
		{
			// Coop client: the server's authoritative money for this body (trade, rewards).
			const u32 money = P.r_u32();
			if (IsGameTypeCoop() && OnClient())
			{
				set_money(money, false);
				if (CurrentGameUI() && Level().CurrentViewEntity() == this)
					CurrentGameUI()->GetActorMenu().CoopMoneyChanged();
			}
		}
		break;
	case GE_COOP_LEVEL_INVITE:
		{
			// Coop client: a level changer on the server invites this player; the SP dialog decides
			// and its OK sends M_CHANGE_LEVEL to the server, which moves everyone together.
			GameGraph::_GRAPH_ID game_vertex = GameGraph::_GRAPH_ID(P.r_u16());
			const u32 level_vertex = P.r_u32();
			Fvector position, angles, reject_position, reject_angles;
			P.r_vec3(position);
			P.r_vec3(angles);
			const bool enabled = !!P.r_u8();
			shared_str invite;
			P.r_stringZ(invite);
			const bool has_reject = !!P.r_u8();
			P.r_vec3(reject_position);
			P.r_vec3(reject_angles);
			CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
			if (ui && IsGameTypeCoop() && OnClient() && Level().CurrentViewEntity() == this)
			{
				Msg("[COOP_CLIENT] LEVEL_INVITE vertex=%u enabled=%u at=%f,%f,%f to=%f,%f,%f reject=%u", u32(game_vertex), enabled ? 1 : 0,
				    VPUSH(Position()), VPUSH(position), has_reject ? 1 : 0);
				ui->ChangeLevel(game_vertex, level_vertex, position, angles, reject_position, reject_angles, has_reject, invite, enabled);
			}
		}
		break;
	case GE_COOP_USE_OBJECT:
		{
			// Coop server: the owning client pressed "use" on a script-usable object (door, lever).
			// Its Lua use_callback(obj, who) runs with this body as the actor.
			const u16 object_id = P.r_u16();
			CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(object_id));
			CUsableScriptObject* usable = object ? smart_cast<CUsableScriptObject*>(object) : NULL;
			// A downed teammate: "use" starts the revive (game_sv_Coop::ReviveStart).
			CActor* downed = object && game_sv_Coop::IsDowned(object_id) ? smart_cast<CActor*>(object) : NULL;
			if (IsGameTypeCoop() && OnServer() && g_Alive() && downed && !object->getDestroy())
			{
				static_cast<game_sv_Coop*>(Level().Server->game)->ReviveStart(this, downed);
			}
			else if (IsGameTypeCoop() && OnServer() && g_Alive() && this != Level().CurrentControlEntity() && usable && !object->getDestroy())
			{
				CoopLuaActor coop_actor(this, false);
				const bool used = usable->use(this);
				if (strstr(Core.Params, "-coop_damage_probe"))
					Msg("[COOP_USE] side=server body=%u object=%u name=%s used=%u tip=%s", ID(), object_id, object->cName().c_str(), used ? 1 : 0,
					    usable->tip_text() ? usable->tip_text() : "<null>");
			}
		}
		break;
	case GE_MOVE_ACTOR:
		{
			Fvector NewPos, NewRot;
			P.r_vec3(NewPos);
			P.r_vec3(NewRot);

			MoveActor(NewPos, NewRot);
		}
		break;
	case GE_ACTOR_MAX_POWER:
		{
			conditions().MaxPower();
			conditions().ClearWounds();
		}
		break;
	case GE_ACTOR_MAX_HEALTH:
		{
			SetfHealth(GetMaxHealth());
		}
		break;
	case GEG_PLAYER_ATTACH_HOLDER:
		{
			u16 id = P.r_u16();
			CObject* O = Level().Objects.net_Find(id);
			if (!O)
			{
				Msg("! Error: No object to attach holder [%d]", id);
				break;
			}
			VERIFY(m_holder==NULL);
			CHolderCustom* holder = smart_cast<CHolderCustom*>(O);
			if (!holder->Engaged()) use_Holder(holder);
		}
		break;
	case GEG_PLAYER_DETACH_HOLDER:
		{
			if (!m_holder) break;
			u16 id = P.r_u16();
			CGameObject* GO = smart_cast<CGameObject*>(m_holder);
			VERIFY(id==GO->ID());
			use_Holder(NULL);
		}
		break;
	case GEG_PLAYER_PLAY_HEADSHOT_PARTICLE:
		{
			OnPlayHeadShotParticle(P);
		}
		break;
	case GE_ACTOR_JUMPING:
		{
			/*
			Fvector dir;
			P.r_dir(dir);
			float jump = P.r_float();
			NET_SavedAccel = dir;
			extern float NET_Jump;
			NET_Jump = jump;
			m_bInInterpolation = false;
			mstate_real |= mcJump;
			*/
		}
		break;
	}
}

void CActor::MoveActor(Fvector NewPos, Fvector NewDir)
{
	Fmatrix M = XFORM();
	M.translate(NewPos);
	r_model_yaw = NewDir.y;
	r_torso.yaw = NewDir.y;
	r_torso.pitch = -NewDir.x;
	unaffected_r_torso.yaw = r_torso.yaw;
	unaffected_r_torso.pitch = r_torso.pitch;
	unaffected_r_torso.roll = 0; //r_torso.roll;

	r_torso_tgt_roll = 0;
	cam_Active()->Set(-unaffected_r_torso.yaw, unaffected_r_torso.pitch, unaffected_r_torso.roll);
	ForceTransform(M);

	m_bInInterpolation = false;
}
