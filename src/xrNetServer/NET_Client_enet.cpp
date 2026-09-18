#include "stdafx.h"

#ifdef XR_USE_ENET

#include "net_client.h"
#include "net_server.h" // SClientConnectData, ip_address
#include "NET_ENet.h"
#include "NET_Messages.h"

// ---------------------------------------------------------------------------
// Transport-specific half of IPureClient, ENet backend.
//
// Mirrors NET_Server_enet.cpp. The DirectPlay implementation stays in
// NET_Client.cpp, compiled out under XR_USE_ENET.
//
// Handshake: DirectPlay carried SClientConnectData in the player info block that
// SetClientInfo() published before joining. ENet has no payload on the connect
// event, so the data goes out as the first packet on chan_handshake, before any
// other traffic.
// ---------------------------------------------------------------------------

namespace
{
	// How long Connect() blocks waiting for the peer to come up.
	const enet_uint32 connect_timeout_ms = 5000;
}

//------------------------------------------------------------------------------
BOOL IPureClient::Connect(LPCSTR options)
{
	if (!xr_enet::initialize())
		return FALSE;

	Disconnect(); // drop whatever is left from a previous session
    net_Connected = EnmConnectionWait;
    net_Syncronised = FALSE;
    net_Disconnected = FALSE;

	// -- options ------------------------------------------------------------
	string256 server_name = "";
	if (strchr(options, '/'))
		strncpy_s(server_name, options, strchr(options, '/') - options);
	else
		xr_strcpy(server_name, options);
	if (strchr(server_name, '/')) *strchr(server_name, '/') = 0;
	if (!xr_strlen(server_name))
		xr_strcpy(server_name, "localhost");

	// The engine appends "/port=<server port>" in CLevel::net_start3; that is the
	// key to read here. "portsv=" is the server side's own spelling and never
	// reaches the client options.
	u32 server_port = START_PORT_LAN_SV;
	if (strstr(options, "port="))
	{
		string64 tmp = "";
		const char* p = strstr(options, "port=") + 5;
		if (strchr(p, '/'))
			strncpy_s(tmp, p, strchr(p, '/') - p);
		else
			strncpy_s(tmp, p, 63);
		const u32 asked = atol(tmp);
		if (asked > 0 && asked < 65536)
			server_port = asked;
		else
			Msg("! ENet: port=%d is not a port, staying on %d", asked, server_port);
	}

	string64 user_name_str = "";
	if (strstr(options, "name="))
	{
		const char* p = strstr(options, "name=") + 5;
		if (strchr(p, '/'))
			strncpy_s(user_name_str, p, strchr(p, '/') - p);
		else
			strncpy_s(user_name_str, p, 63);
	}
	if (!xr_strlen(user_name_str))
		xr_strcpy(user_name_str, psNET_Name);

	string64 user_pass = "";
	if (strstr(options, "psw="))
	{
		const char* p = strstr(options, "psw=") + 4;
		if (strchr(p, '/'))
			strncpy_s(user_pass, p, strchr(p, '/') - p);
		else
			strncpy_s(user_pass, p, 63);
	}

	// Coop: the character chosen in the join menu rides on the handshake (SClientConnectData::coop).
	char coop_profile[768] = "";
	if (strstr(options, "/coop="))
	{
		const char* p = strstr(options, "/coop=") + 6;
		const size_t n = strchr(p, '/') ? size_t(strchr(p, '/') - p) : xr_strlen(p);
		strncpy_s(coop_profile, p, _min(n, sizeof(coop_profile) - 1));
	}

	// -- host ---------------------------------------------------------------
	// Client host: one outgoing connection, no bound port of our own.
	m_enet_host = enet_host_create(NULL, 1, xr_enet::chan_count, 0, 0);
	if (!m_enet_host)
	{
		Msg("! ENet: cannot create client host");
		return FALSE;
	}

	ENetAddress address;
	if (enet_address_set_host(&address, server_name) != 0)
	{
		Msg("! ENet: cannot resolve host [%s]", server_name);
		enet_host_destroy(m_enet_host);
		m_enet_host = NULL;
		OnInvalidHost();
		return FALSE;
	}
	address.port = enet_uint16(server_port);

	m_enet_peer = enet_host_connect(m_enet_host, &address, xr_enet::chan_count, 0);
	if (!m_enet_peer)
	{
		Msg("! ENet: no free peer for outgoing connection");
		enet_host_destroy(m_enet_host);
		m_enet_host = NULL;
		return FALSE;
	}
	// The server's silence tolerance, mirrored (see NET_Server_enet.cpp): a server saving the world
	// or loading a level for seconds must not look dead to this client.
	enet_peer_timeout(m_enet_peer, 32, 30000, 120000);

	// -- wait for the link --------------------------------------------------
	ENetEvent e;
	bool established = false;
	const enet_uint32 deadline = enet_time_get() + connect_timeout_ms;
	while (enet_time_get() < deadline)
	{
		xr_enet::pump_local(); // the server may well be running in this process

		if (enet_host_service(m_enet_host, &e, 10) > 0)
		{
			if (e.type == ENET_EVENT_TYPE_CONNECT)
			{
				established = true;
				break;
			}
			if (e.type == ENET_EVENT_TYPE_RECEIVE)
				enet_packet_destroy(e.packet);
			if (e.type == ENET_EVENT_TYPE_DISCONNECT)
				break;
		}
	}

	if (!established)
	{
		Msg("! ENet: connection to [%s:%d] failed", server_name, server_port);
		enet_peer_reset(m_enet_peer);
		m_enet_peer = NULL;
		enet_host_destroy(m_enet_host);
		m_enet_host = NULL;
		OnConnectRejected();
		return FALSE;
	}

	// -- handshake ----------------------------------------------------------
	SClientConnectData cl_data;
	cl_data.process_id = GetCurrentProcessId();
	xr_strcpy(cl_data.name, user_name_str);
	xr_strcpy(cl_data.pass, user_pass);
	xr_strcpy(cl_data.coop, coop_profile);

	ENetPacket* hs = enet_packet_create(&cl_data, sizeof(cl_data), ENET_PACKET_FLAG_RELIABLE);
	if (!hs || enet_peer_send(m_enet_peer, xr_enet::chan_handshake, hs) < 0)
	{
		if (hs) enet_packet_destroy(hs);
		Msg("! ENet: cannot send handshake");
		Disconnect();
		return FALSE;
	}
	enet_host_flush(m_enet_host);

	// The server answers with the game description; the level cannot be looked up
	// without it, so the link is not usable until it arrives.
	bool described = false;
	const enet_uint32 descr_deadline = enet_time_get() + connect_timeout_ms;
	while (!described && enet_time_get() < descr_deadline)
	{
		xr_enet::pump_local();

		while (enet_host_service(m_enet_host, &e, 10) > 0)
		{
			if (e.type == ENET_EVENT_TYPE_RECEIVE)
			{
				if (e.channelID == xr_enet::chan_handshake &&
					e.packet->dataLength == sizeof(m_game_description))
				{
					CopyMemory(&m_game_description, e.packet->data, sizeof(m_game_description));
					described = true;
				}
				else
				{
					// game traffic can legitimately overtake the description
					MultipacketReciever::RecievePacket(e.packet->data, u32(e.packet->dataLength));
				}
				enet_packet_destroy(e.packet);
			}
			else if (e.type == ENET_EVENT_TYPE_DISCONNECT)
			{
				Msg("! ENet: server dropped us during the handshake");
				m_enet_peer = NULL;
				Disconnect();
				return FALSE;
			}
		}
	}

	if (!described)
	{
		Msg("! ENet: server never sent the game description");
		Disconnect();
		return FALSE;
	}

	Msg("* ENet: connected to %s:%d as [%s], map [%s] version [%s]",
	    server_name, server_port, user_name_str,
	    m_game_description.map_name, m_game_description.map_version);

    // An external server can send MSYS_CONFIG while we wait for its description.
    // RecievePacket has already advanced net_Connected in that case. Never
    // reset it here, or subsequent authentication packets will be discarded.
    Msg("* ENet: description ready, config_state=%u", u32(net_Connected));
    FlushLog();

	return TRUE;
}

//------------------------------------------------------------------------------
void IPureClient::Disconnect()
{
	if (!m_enet_host)
	{
		net_Connected = EnmConnectionWait;
		net_Syncronised = FALSE;
		return;
	}

	if (m_enet_peer && m_enet_peer->state == ENET_PEER_STATE_CONNECTED)
	{
		enet_peer_disconnect(m_enet_peer, 0);

		ENetEvent e;
		const enet_uint32 deadline = enet_time_get() + 1000;
		while (enet_time_get() < deadline)
		{
			xr_enet::pump_local();

			if (enet_host_service(m_enet_host, &e, 10) > 0)
			{
				if (e.type == ENET_EVENT_TYPE_RECEIVE)
					enet_packet_destroy(e.packet);
				else if (e.type == ENET_EVENT_TYPE_DISCONNECT)
					break;
			}
		}
	}

	if (m_enet_peer)
	{
		enet_peer_reset(m_enet_peer);
		m_enet_peer = NULL;
	}

	enet_host_destroy(m_enet_host);
	m_enet_host = NULL;

	net_Connected = EnmConnectionWait;
	net_Syncronised = FALSE;
}

//------------------------------------------------------------------------------
// Pump. Replaces the DirectPlay cases the client cared about:
//   RECEIVE           -> MultipacketReciever::RecievePacket
//   TERMINATE_SESSION -> net_Disconnected + OnSessionTerminate
// ENUM_HOSTS_RESPONSE (LAN browse) has no equivalent yet; direct connect only.
void IPureClient::Poll()
{
	if (!m_enet_host)
		return;

	ENetEvent e;
	while (enet_host_service(m_enet_host, &e, 0) > 0)
	{
		switch (e.type)
		{
		case ENET_EVENT_TYPE_RECEIVE:
			MultipacketReciever::RecievePacket(e.packet->data, u32(e.packet->dataLength));
			enet_packet_destroy(e.packet);
			break;

		case ENET_EVENT_TYPE_DISCONNECT:
			{
                Msg("[ENET_CLIENT] PEER_LEFT data=%u time=%u", e.data, enet_time_get());
				net_Disconnected = TRUE;
				m_enet_peer = NULL;

				LPCSTR reason = "Connection closed";
				switch (e.data)
				{
				case 1: reason = "You are banned on this server";
					break;
				case 2: reason = "Server is not for your subnet";
					break;
				case 3: reason = "Handshake rejected";
					break;
				default: break;
				}
				OnSessionTerminate(reason);
			}
			break;

		default:
			break;
		}
	}

	sync_step();
}

//------------------------------------------------------------------------------
void IPureClient::SendTo_LL(void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
	if (!m_enet_host || !m_enet_peer || m_enet_peer->state != ENET_PEER_STATE_CONNECTED)
		return;

	ENetPacket* packet = enet_packet_create(data, size, xr_enet::packet_flags_of(dwFlags));
	if (!packet)
		return;

	if (enet_peer_send(m_enet_peer, xr_enet::channel_of(dwFlags), packet) < 0)
	{
		enet_packet_destroy(packet);
		return;
	}

	net_Statistic.dwBytesSended += size;

	// DirectPlay's DPNSEND_IMMEDIATELLY had no queueing delay; match it.
	if (dwFlags & DPNSEND_IMMEDIATELLY)
		enet_host_flush(m_enet_host);
}

//------------------------------------------------------------------------------
BOOL IPureClient::net_HasBandwidth()
{
	u32 dwTime = TimeGlobal(device_timer);
	u32 dwInterval = 0;

	if (psNET_direct_connect)
		return TRUE;

	if (psNET_ClientUpdate != 0) dwInterval = 1000 / psNET_ClientUpdate;
	if (psNET_Flags.test(NETFLAG_MINIMIZEUPDATES)) dwInterval = 1000;

	if (0 != psNET_ClientUpdate && (dwTime - net_Time_LastUpdate) > dwInterval)
	{
		if (!m_enet_peer)
			return FALSE;

		// Same backpressure signal as the server side.
		if (m_enet_peer->reliableDataInTransit >= m_enet_peer->windowSize)
		{
			net_Statistic.dwTimesBlocked++;
			return FALSE;
		}

		UpdateStatistic();
		net_Time_LastUpdate = dwTime;
		return TRUE;
	}
	return FALSE;
}

//------------------------------------------------------------------------------
void IPureClient::UpdateStatistic()
{
	DPN_CONNECTION_INFO CI;
	xr_enet::fill_connection_info(m_enet_peer, CI);
	net_Statistic.Update(CI);
}

//------------------------------------------------------------------------------
bool IPureClient::GetServerAddress(ip_address& pAddress, DWORD* pPort)
{
	if (!m_enet_peer)
		return false;

	pAddress.m_data.data = m_enet_peer->address.host;
	if (pPort)
		*pPort = m_enet_peer->address.port;
	return true;
}

#endif // XR_USE_ENET
