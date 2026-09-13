#include "stdafx.h"
#include "xrserver.h"
#include "xrmessages.h"
#include "xrserver_objects.h"
#include "xrServer_Objects_Alife_Monsters.h"
#include "Level.h"
#include "game_sv_coop.h"


void xrServer::Perform_connect_spawn(CSE_Abstract* E, xrClientData* CL, NET_Packet& P)
{
	P.B.count = 0;
    if (game->Type() == eGameIDCoop && CL != GetServerClient())
    {
        // The internal actor and its inventory are server services, not replicas.
        for (CSE_Abstract* ancestor = E; ancestor; ancestor = ID_to_entity(ancestor->ID_Parent))
            if (ancestor->owner == GetServerClient() && ancestor->s_flags.is(M_SPAWN_OBJECT_ASPLAYER))
            {
                Msg("[COOP_SERVER] ANCHOR_NOT_STREAMED client=%u object=%u", CL->ID.value(), E->ID);
                return;
            }
    }
	xr_vector<u16>::iterator it = std::find(conn_spawned_ids.begin(), conn_spawned_ids.end(), E->ID);
	if (it != conn_spawned_ids.end())
	{
		//.		Msg("Rejecting redundant SPAWN data [%d]", E->ID);
		return;
	}

	conn_spawned_ids.push_back(E->ID);

	if (E->net_Processed) return;
	if (E->s_flags.is(M_SPAWN_OBJECT_PHANTOM)) return;

	if (game->Type() == eGameIDCoop && strstr(Core.Params, "-coop_damage_probe") && smart_cast<CSE_ALifeItemWeapon*>(E))
		Msg("[COOP_CONNECT_SPAWN] client=%u item=%u section=%s parent=%u", CL->ID.value(), E->ID, E->s_name.c_str(), E->ID_Parent);
	//.	Msg("Perform connect spawn [%d][%s]", E->ID, E->s_name.c_str());

	// Connectivity order
	CSE_Abstract* Parent = ID_to_entity(E->ID_Parent);
	if (Parent) Perform_connect_spawn(Parent, CL, P);

	// Process
	Flags16 save = E->s_flags;
	//-------------------------------------------------
	E->s_flags.set(M_SPAWN_UPDATE,TRUE);
	if (0 == E->owner)
	{
		// PROCESS NAME; Name this entity
		if (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER))
		{
			CL->owner = E;
			VERIFY(CL->ps);
			E->set_name_replace(CL->ps->getName());
		}

		// Associate
		E->owner = CL;
		E->Spawn_Write(P,TRUE);
		E->UPDATE_Write(P);

		CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(E);
		VERIFY(object);
		if (!object->keep_saved_data_anyway())
			object->client_data.clear();
	}
	else
	{
		WriteOwnedConnectSpawn(E, CL, P);
		//		CSE_ALifeObject*	object = smart_cast<CSE_ALifeObject*>(E);
		//		VERIFY				(object);
		//		VERIFY				(object->client_data.empty());
	}
	//-----------------------------------------------------
	E->s_flags = save;
	SendTo(CL->ID, P, net_flags(TRUE,TRUE));
	E->net_Processed = TRUE;
}

// Only connection serialization changes. CSE ownership and the ALife world
// actor flag must never follow the recipient's control assignment.
void xrServer::WriteOwnedConnectSpawn(CSE_Abstract* E, xrClientData* to, NET_Packet& P)
{
    const Flags16 saved = E->s_flags;
    const bool controlled = game->Type() == eGameIDCoop &&
        GetServerClient() && E->owner == GetServerClient() && to != GetServerClient() &&
        to->owner == E && smart_cast<CSE_ALifeCreatureActor*>(E) &&
        !E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER);
    // Spawn_Write is synchronous; restore even VERSION, which it sets itself.
    // No ownership transfer, callback, queued work, or world registration here.
    if (controlled) E->s_flags.set(M_SPAWN_OBJECT_ASPLAYER, TRUE);
    {
        CoopHideClientData hide(E);
        E->Spawn_Write(P, controlled ? TRUE : FALSE);
    }
    E->UPDATE_Write(P);
    E->s_flags = saved;
}

xrServer::CoopHideClientData::CoopHideClientData(CSE_Abstract* E) : object(E)
{
    if (object && !object->client_data.empty())
        saved.swap(object->client_data);
}

xrServer::CoopHideClientData::~CoopHideClientData()
{
    if (object && !saved.empty())
        object->client_data.swap(saved);
}

void xrServer::TestCoopSpawnRouting(CSE_Abstract* body, CSE_Abstract* world)
{
    R_ASSERT(game->Type() == eGameIDCoop && body && world && body != world);
    R_ASSERT(body->owner == GetServerClient() && world->owner == GetServerClient());
    xrClientData controller, observer;
    controller.owner = body;
    const auto check = [&](CSE_Abstract* object, xrClientData* recipient, bool controlled)
    {
        const Flags16 saved = object->s_flags;
        xrClientData* owner = object->owner;
        NET_Packet actual, legacy;
        WriteOwnedConnectSpawn(object, recipient, actual);
        R_ASSERT(object->s_flags.flags == saved.flags && object->owner == owner);
        object->Spawn_Write(legacy, FALSE);
        object->UPDATE_Write(legacy);
        object->s_flags = saved;
        // Decode the common spawn header independently of the writer.
        u16 type;
        actual.r_begin(type);
        R_ASSERT(type == M_SPAWN);
        shared_str section, name;
        actual.r_stringZ(section); actual.r_stringZ(name);
        actual.r_u8(); actual.r_u8();
        Fvector position, angle;
        actual.r_vec3(position); actual.r_vec3(angle);
        actual.r_u16();
        R_ASSERT(actual.r_u16() == object->ID);
        actual.r_u16(); actual.r_u16();
        const u32 offset = actual.r_tell();
        const u16 flags = actual.r_u16();
        const u16 control_flags = M_SPAWN_OBJECT_LOCAL | M_SPAWN_OBJECT_ASPLAYER;
        R_ASSERT((flags & control_flags) == (controlled ? control_flags : 0));
        R_ASSERT(actual.B.count == legacy.B.count);
        // After masking just the two intended bits, every byte must match
        // the native remote packet, including UPDATE and child/custom data.
        const u16 masked = flags & ~control_flags;
        actual.w_seek(offset, &masked, sizeof(masked));
        R_ASSERT(memcmp(actual.B.data, legacy.B.data, actual.B.count) == 0);
    };
    check(body, &controller, true);
    check(world, &controller, false);
    check(body, &observer, false);
    check(body, static_cast<xrClientData*>(GetServerClient()), false);
    controller.owner = world;
    check(world, &controller, false); // Never hand out the world protagonist.
    controller.owner = body;
    check(body, &controller, true); // No leak from the previous recipient.
    R_ASSERT(!body->children.empty());
    CSE_Abstract* child = ID_to_entity(body->children.front());
    R_ASSERT(child && !smart_cast<CSE_ALifeCreatureActor*>(child));
    controller.owner = child;
    check(child, &controller, false); // An item cannot become a control actor.
    R_ASSERT(!body->s_flags.is(M_SPAWN_OBJECT_ASPLAYER));
    R_ASSERT(world->s_flags.is(M_SPAWN_OBJECT_ASPLAYER));
    R_ASSERT(body->owner == GetServerClient() && world->owner == GetServerClient());
    Msg("[COOP_SERVER] SPAWN_ROUTING_PASS body=%u world=%u cases=7 owner_unchanged=1 native_bytes=1", body->ID, world->ID);
}

void xrServer::SendConfigFinished(ClientID const& clientId)
{
	NET_Packet P;
	P.w_begin(M_SV_CONFIG_FINISHED);
	SendTo(clientId, P, net_flags(TRUE,TRUE));
}

void xrServer::SendConnectionData(IClient* _CL)
{
	conn_spawned_ids.clear();
	xrClientData* CL = (xrClientData*)_CL;
	NET_Packet P;
	// Replicate current entities on to this client
	xrS_entities::iterator I = entities.begin(), E = entities.end();
	for (; I != E; ++I) I->second->net_Processed = FALSE;
	for (I = entities.begin(); I != E; ++I) Perform_connect_spawn(I->second, CL, P);

	// Start to send server logo and rules
	SendServerInfoToClient(CL->ID);

	/*
		Msg("--- Our sended SPAWN IDs:");
		xr_vector<u16>::iterator it = conn_spawned_ids.begin();
		for (; it != conn_spawned_ids.end(); ++it)
		{
			Msg("%d", *it);
		}
		Msg("---- Our sended SPAWN END");
	*/
};

void xrServer::OnCL_Connected(IClient* _CL)
{
	xrClientData* CL = (xrClientData*)_CL;
	if (game->Type() == eGameIDCoop)
		static_cast<game_sv_Coop*>(game)->PrepareClient(CL);
	CL->net_Accepted = TRUE;
	/*if (Level().IsDemoPlay())
	{
		Level().StartPlayDemo();
		return;
	};*/
	///	Server_Client_Check(CL);
	//csPlayers.Enter					();	//sychronized by a parent call
	Export_game_type(CL);
	Perform_game_export();
	SendConnectionData(CL);

	VERIFY2(CL->ps, "Player state not created");
	if (!CL->ps)
	{
		Msg("! ERROR: Player state not created - incorect message sequence!");
		return;
	}

	game->OnPlayerConnect(CL->ID);
}

void xrServer::SendConnectResult(IClient* CL, u8 res, u8 res1, char* ResultStr)
{
	NET_Packet P;
	P.w_begin(M_CLIENT_CONNECT_RESULT);
	P.w_u8(res);
	P.w_u8(res1);
	P.w_stringZ(ResultStr);
	P.w_clientID(CL->ID);

	if (SV_Client && SV_Client == CL)
		P.w_u8(1);
	else
		P.w_u8(0);
	P.w_stringZ(Level().m_caServerOptions);

	SendTo(CL->ID, P);

	if (!res) //need disconnect 
	{
#ifdef MP_LOGGING
		Msg("* Server disconnecting client, resaon: %s", ResultStr);
#endif
		Flush_Clients_Buffers();
		DisconnectClient(CL, ResultStr);
	}

	if (Level().IsDemoPlay())
	{
		Level().StartPlayDemo();

		return;
	}
};

void xrServer::SendProfileCreationError(IClient* CL, char const* reason)
{
	VERIFY(CL);

	NET_Packet P;
	P.w_begin(M_CLIENT_CONNECT_RESULT);
	P.w_u8(0);
	P.w_u8(ecr_profile_error);
	P.w_stringZ(reason);
	P.w_clientID(CL->ID);
	SendTo(CL->ID, P);
	if (CL != GetServerClient())
	{
		Flush_Clients_Buffers();
		DisconnectClient(CL, reason);
	}
}

//this method response for client validation on connect state (CLevel::net_start_client2)
//the first validation is CDKEY, then gamedata checksum (NeedToCheckClient_BuildVersion), then 
//banned or not...
//WARNING ! if you will change this method see M_AUTH_CHALLENGE event handler
void xrServer::Check_GameSpy_CDKey_Success(IClient* CL)
{
	if (NeedToCheckClient_BuildVersion(CL))
		return;
	//-------------------------------------------------------------
	RequestClientDigest(CL);
};

BOOL g_SV_Disable_Auth_Check = FALSE;

bool xrServer::NeedToCheckClient_BuildVersion(IClient* CL)
{
	/*#ifdef DEBUG
	
		return false; 
	
	#endif*/
	xrClientData* tmp_client = smart_cast<xrClientData*>(CL);
	VERIFY(tmp_client);
	PerformSecretKeysSync(tmp_client);


	if (g_SV_Disable_Auth_Check) return false;
	CL->flags.bVerified = FALSE;
	NET_Packet P;
	P.w_begin(M_AUTH_CHALLENGE);
	SendTo(CL->ID, P);
	return true;
};

void xrServer::OnBuildVersionRespond(IClient* CL, NET_Packet& P)
{
	u16 Type;
	P.r_begin(Type);
	u64 _our = FS.auth_get();
    if (game->Type()==eGameIDCoop) _our ^= 0x434F4F50414E4902ull;
	u64 _him = P.r_u64();

#ifdef USE_DEBUG_AUTH
	Msg("_our = %d", _our);
	Msg("_him = %d", _him);
	_our = MP_DEBUG_AUTH;
#endif // USE_DEBUG_AUTH

	if (_our != _him)
	{
		SendConnectResult(CL, 0, ecr_data_verification_failed, "Data verification failed. Cheater?");
	}
	else
	{
		bool bAccessUser = false;
		string512 res_check;

		if (!CL->flags.bLocal)
		{
			bAccessUser = Check_ServerAccess(CL, res_check);
		}

		if (CL->flags.bLocal || bAccessUser)
		{
			//Check_BuildVersion_Success( CL );
			RequestClientDigest(CL);
		}
		else
		{
			Msg("* Client 0x%08x has an incorrect password", CL->ID.value());
			xr_strcat(res_check, "Invalid password.");
			SendConnectResult(CL, 0, ecr_password_verification_failed, res_check);
		}
	}
};

void xrServer::Check_BuildVersion_Success(IClient* CL)
{
	CL->flags.bVerified = TRUE;
	SendConnectResult(CL, 1, 0, "All Ok");
};
