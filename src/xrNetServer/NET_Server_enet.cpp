#include "stdafx.h"

#ifdef XR_USE_ENET

#include "net_server.h"
#include "NET_ENet.h"
#include "NET_Messages.h"
#include "../xrCore/xrCore.h"

// ---------------------------------------------------------------------------
// Transport-specific half of IPureServer, ENet backend.
//
// Everything else in NET_Server.cpp (client bookkeeping, ban lists, statistics,
// bandwidth, the OnMessage/OnCL_* defaults) is transport agnostic and is used
// as is.
//
// Difference from the DirectPlay implementation that callers must know about:
// DirectPlay delivered messages on its own threads, ENet is polled. Poll() has
// to be pumped from the server's per-frame Update().
// ---------------------------------------------------------------------------

namespace
{
	// Per-peer state. DirectPlay handed us SClientConnectData inside the
	// CREATE_PLAYER message; with ENet the client sends it as its first packet
	// on chan_handshake, so a peer lives in "connected but unannounced" state
	// until that arrives.
	struct peer_state
	{
		bool announced;
		u32 client_id;
	};

	IC peer_state* state_of(ENetPeer* peer) { return static_cast<peer_state*>(peer->data); }
}

bool xr_enet::initialize()
{
	static bool ready = false;
	static bool failed = false;

	if (ready) return true;
	if (failed) return false;

	if (enet_initialize() != 0)
	{
		Msg("! ENet: enet_initialize failed");
		failed = true;
		return false;
	}
	atexit(enet_deinitialize);
	ready = true;
	return true;
}

void xr_enet::fill_connection_info(const ENetPeer* peer, DPN_CONNECTION_INFO& ci)
{
	ZeroMemory(&ci, sizeof(ci));
	ci.dwSize = sizeof(ci);

	if (!peer)
		return;

	ci.dwRoundTripLatencyMS = peer->roundTripTime;
	// ENet accounts window size in bytes per RTT; convert to bytes per second.
	if (peer->roundTripTime)
		ci.dwThroughputBPS = u32((u64(peer->windowSize) * 1000) / peer->roundTripTime);
	ci.dwPeakThroughputBPS = ci.dwThroughputBPS;
	ci.dwPacketsDropped = peer->packetsLost;
	ci.dwPacketsRetried = peer->packetsLost; // ENet does not split these out
	ci.dwMessagesReceived = peer->incomingDataTotal;
	ci.dwMessagesTransmittedNormalPriority = peer->packetsSent;
}

//------------------------------------------------------------------------------
// See xr_enet::set_local_pump: a client blocking inside this same process needs
// the co-hosted server serviced meanwhile, or neither side ever moves.
static IPureServer* g_local_server = NULL;
static xr_enet::pump_proc g_local_pump = NULL;
static bool g_pumping = false;

static void pump_local_server()
{
	if (g_local_server)
		g_local_server->Poll();
}

void xr_enet::set_local_pump(xr_enet::pump_proc pump)
{
	if (!pump)
		g_local_server = NULL;
	g_local_pump = pump;
}

void xr_enet::pump_local()
{
	// Poll() dispatches messages, which must never re-enter the pump
	if (!g_local_pump || g_pumping)
		return;

	g_pumping = true;
	g_local_pump();
	g_pumping = false;
}

IPureServer::EConnect IPureServer::Connect(LPCSTR options, GameDescriptionData& game_descr)
{
	connect_options = options;
	psNET_direct_connect = FALSE;

	if (strstr(options, "/single"))
		psNET_direct_connect = TRUE;

	if (!xr_enet::initialize())
		return ErrConnect;

	// -- options ------------------------------------------------------------
	string4096 session_name;
	xr_strcpy(session_name, options);
	if (strchr(session_name, '/')) *strchr(session_name, '/') = 0;

	u32 dwMaxPlayers = 32;
	if (strstr(options, "maxplayers="))
	{
		string64 tmp = "";
		const char* p = strstr(options, "maxplayers=") + 11;
		if (strchr(p, '/'))
			strncpy_s(tmp, p, strchr(p, '/') - p);
		else
			strncpy_s(tmp, p, 63);
		dwMaxPlayers = atol(tmp);
	}
	if (dwMaxPlayers > 32 || dwMaxPlayers < 1) dwMaxPlayers = 32;

	u32 dwServerPort = START_PORT_LAN_SV;
	if (strstr(options, "portsv="))
	{
		string64 tmp = "";
		const char* p = strstr(options, "portsv=") + 7;
		if (strchr(p, '/'))
			strncpy_s(tmp, p, strchr(p, '/') - p);
		else
			strncpy_s(tmp, p, 63);
		// Not clamped to START_PORT..END_PORT the way DirectPlay did: that window is
		// two ports wide and quietly rewrites anything else, 1235 included.
		const u32 asked = atol(tmp);
		if (asked > 0 && asked < 65536)
			dwServerPort = asked;
		else
			Msg("! ENet: portsv=%d is not a port, staying on %d", asked, dwServerPort);
	}

	// -- host ---------------------------------------------------------------
	ENetAddress address;
	address.host = ENET_HOST_ANY;

	// A stale process may still hold the port, so walk a few like DirectPlay did.
	const u32 last_port = dwServerPort + 3;
	for (u32 port = dwServerPort; port <= last_port && !m_enet_host; ++port)
	{
		address.port = enet_uint16(port);
		m_enet_host = enet_host_create(&address, dwMaxPlayers, xr_enet::chan_count, 0, 0);
		if (m_enet_host)
			dwServerPort = port;
		else
			Msg("! ENet: port %d is busy", port);
	}

	if (!m_enet_host)
	{
		Msg("! ENet: cannot bind a server socket");
		return ErrConnect;
	}

	// GetPort() feeds the "/port=" the engine appends to the client options; it
	// has to name the port we actually bound, not the one we asked for.
	psNET_Port = int(dwServerPort);

	Msg("* ENet server listening on port %d, max players %d", dwServerPort, dwMaxPlayers);

	m_enet_game_descr = game_descr;

	g_local_server = this;
	xr_enet::set_local_pump(&pump_local_server);

	SV_Client = NULL;
	return ErrNoError;
}

//------------------------------------------------------------------------------
void IPureServer::Disconnect()
{
	if (!m_enet_host)
		return;

	if (g_local_server == this)
		xr_enet::set_local_pump(NULL);

	// Ask everybody to leave, then give ENet a short window to flush.
	for (size_t i = 0; i < m_enet_host->peerCount; ++i)
	{
		ENetPeer* peer = &m_enet_host->peers[i];
		if (peer->state == ENET_PEER_STATE_CONNECTED)
			enet_peer_disconnect(peer, 0);
	}

	ENetEvent e;
	while (enet_host_service(m_enet_host, &e, 200) > 0)
	{
		if (e.type == ENET_EVENT_TYPE_RECEIVE)
			enet_packet_destroy(e.packet);
		else if (e.type == ENET_EVENT_TYPE_DISCONNECT && e.peer->data)
		{
			xr_free(e.peer->data);
			e.peer->data = NULL;
		}
	}

	for (size_t i = 0; i < m_enet_host->peerCount; ++i)
	{
		ENetPeer* peer = &m_enet_host->peers[i];
		if (peer->data)
		{
			xr_free(peer->data);
			peer->data = NULL;
		}
		if (peer->state != ENET_PEER_STATE_DISCONNECTED)
			enet_peer_reset(peer);
	}

	enet_host_destroy(m_enet_host);
	m_enet_host = NULL;
	SV_Client = NULL;
}

//------------------------------------------------------------------------------
// Pump. Reproduces what net_Handler() did for the DirectPlay message types:
//   CREATE_PLAYER  -> new_client()          (deferred until the handshake packet)
//   DESTROY_PLAYER -> OnCL_Disconnected + client_Destroy
//   RECEIVE        -> MSYS_PING reply, else MultipacketReciever::RecievePacket
//   INDICATE_CONNECT -> ban list and subnet filter
void IPureServer::Poll()
{
	if (!m_enet_host)
		return;

	ENetEvent e;
	while (enet_host_service(m_enet_host, &e, 0) > 0)
	{
		switch (e.type)
		{
		case ENET_EVENT_TYPE_CONNECT:
			{
				ip_address addr;
				addr.m_data.data = e.peer->address.host;

				// Same two rejections the DirectPlay INDICATE_CONNECT did.
				if (GetBannedClient(addr))
				{
					enet_peer_disconnect_now(e.peer, 1);
					break;
				}
				if (SV_Client && !m_ip_filter.is_ip_present(addr.m_data.data))
				{
					enet_peer_disconnect_now(e.peer, 2);
					break;
				}

				peer_state* st = static_cast<peer_state*>(xr_malloc(sizeof(peer_state)));
				st->announced = false;
				st->client_id = xr_enet::client_id_of(e.peer);
				e.peer->data = st;
			}
			break;

		case ENET_EVENT_TYPE_DISCONNECT:
			{
				peer_state* st = state_of(e.peer);
				if (st && st->announced)
				{
					IClient* client = net_players.GetFoundClient(
						ClientIdSearchPredicate(ClientID(st->client_id)));
					if (client)
					{
                        Msg("[ENET_SERVER] PEER_LEFT client=%u peer=%u data=%u", st->client_id, e.peer->incomingPeerID, e.data);
						client->flags.bConnected = FALSE;
						client->flags.bReconnect = FALSE;
						OnCL_Disconnected(client);
						client_Destroy(client);
					}
				}
				if (st)
				{
					xr_free(e.peer->data);
					e.peer->data = NULL;
				}
			}
			break;

		case ENET_EVENT_TYPE_RECEIVE:
			{
				peer_state* st = state_of(e.peer);

				if (e.channelID == xr_enet::chan_handshake)
				{
					// The client's opening packet: SClientConnectData.
					if (st && !st->announced && e.packet->dataLength == sizeof(SClientConnectData))
					{
						SClientConnectData cl_data = *reinterpret_cast<SClientConnectData*>(e.packet->data);
						cl_data.clientID.set(st->client_id);
						st->announced = true;
						new_client(&cl_data);

						// Our half of the handshake: which map this session runs.
						ENetPacket* descr = enet_packet_create(&m_enet_game_descr,
						                                      sizeof(m_enet_game_descr),
						                                      ENET_PACKET_FLAG_RELIABLE);
						if (descr && enet_peer_send(e.peer, xr_enet::chan_handshake, descr) < 0)
							enet_packet_destroy(descr);
					}
					else
					{
						Msg("! ENet: malformed handshake from peer %d", st ? st->client_id : 0);
						enet_peer_disconnect_now(e.peer, 3);
					}
					enet_packet_destroy(e.packet);
					break;
				}

				if (!st || !st->announced)
				{
					// Data before the handshake -- drop it rather than feed the
					// multipacket layer from an unknown sender.
					enet_packet_destroy(e.packet);
					break;
				}

				void* m_data = e.packet->data;
				u32 m_size = u32(e.packet->dataLength);
				MSYS_PING* m_ping = (MSYS_PING*)m_data;

				if ((m_size > 2 * sizeof(u32)) && (m_ping->sign1 == 0x12071980) && (m_ping->sign2 == 0x26111975))
				{
					if (m_size == sizeof(MSYS_PING))
					{
						m_ping->dwTime_Server = TimerAsync(device_timer);
						ClientID id(st->client_id);
						IPureServer::SendTo_Buf(id, m_data, m_size, net_flags(FALSE, FALSE, TRUE, TRUE));
					}
				}
				else
				{
					MultipacketReciever::RecievePacket(m_data, m_size, st->client_id);
				}

				enet_packet_destroy(e.packet);
			}
			break;

		default:
			break;
		}
	}
}

//------------------------------------------------------------------------------
ENetPeer* IPureServer::peer_of(ClientID id)
{
	if (!m_enet_host || !id.value())
		return NULL;

	const u32 slot = id.value() - 1;
	if (slot >= m_enet_host->peerCount)
		return NULL;

	ENetPeer* peer = &m_enet_host->peers[slot];
	return (peer->state == ENET_PEER_STATE_CONNECTED) ? peer : NULL;
}

//------------------------------------------------------------------------------
void IPureServer::SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
	ENetPeer* peer = peer_of(ID);
	if (!peer)
		return;

	ENetPacket* packet = enet_packet_create(data, size, xr_enet::packet_flags_of(dwFlags));
	if (!packet)
		return;

	if (enet_peer_send(peer, xr_enet::channel_of(dwFlags), packet) < 0)
		enet_packet_destroy(packet);
}

//------------------------------------------------------------------------------
void IPureServer::SendBroadcast_LL(ClientID exclude, void* data, u32 size, u32 dwFlags)
{
	if (!m_enet_host)
		return;

	for (size_t i = 0; i < m_enet_host->peerCount; ++i)
	{
		ENetPeer* peer = &m_enet_host->peers[i];
		if (peer->state != ENET_PEER_STATE_CONNECTED)
			continue;

		peer_state* st = state_of(peer);
		if (!st || !st->announced)
			continue;
		if (exclude.value() && st->client_id == exclude.value())
			continue;

		ENetPacket* packet = enet_packet_create(data, size, xr_enet::packet_flags_of(dwFlags));
		if (!packet)
			continue;
		if (enet_peer_send(peer, xr_enet::channel_of(dwFlags), packet) < 0)
			enet_packet_destroy(packet);
	}
}

//------------------------------------------------------------------------------
// DirectPlay gauged backpressure by the send-queue depth (GetSendQueueInfo).
// ENet's equivalent signal is how much reliable data is still unacknowledged;
// once that reaches the peer's window there is no point queueing more.
BOOL IPureServer::HasBandwidth(IClient* C)
{
	u32 dwTime = TimeGlobal(device_timer);
	u32 dwInterval = 0;

	if (psNET_direct_connect)
	{
		UpdateClientStatistic(C);
		C->dwTime_LastUpdate = dwTime;
		return TRUE;
	}

	if (psNET_ServerUpdate != 0) dwInterval = 1000 / psNET_ServerUpdate;
	if (psNET_Flags.test(NETFLAG_MINIMIZEUPDATES)) dwInterval = 1000;

	if (psNET_ServerUpdate != 0 && (dwTime - C->dwTime_LastUpdate) > dwInterval)
	{
		ENetPeer* peer = peer_of(C->ID);
		if (!peer)
			return FALSE;

		if (peer->reliableDataInTransit >= peer->windowSize)
		{
			C->stats.dwTimesBlocked++;
			return FALSE;
		}

		UpdateClientStatistic(C);
		C->dwTime_LastUpdate = dwTime;
		return TRUE;
	}
	return FALSE;
}

//------------------------------------------------------------------------------
void IPureServer::UpdateClientStatistic(IClient* C)
{
	DPN_CONNECTION_INFO CI;
	xr_enet::fill_connection_info(peer_of(C->ID), CI);
	C->stats.Update(CI);
}

//------------------------------------------------------------------------------
bool IPureServer::DisconnectClient(IClient* C, LPCSTR Reason)
{
	if (!C) return false;
    Msg("[ENET_SERVER] DISCONNECT_REQUEST client=%u reason=%s", C->ID.value(), Reason ? Reason : "");

	ENetPeer* peer = peer_of(C->ID);
	if (!peer) return false;

	// The reason string cannot ride along on an ENet disconnect (it carries a
	// single u32), so send it as a last reliable message before tearing down.
	if (Reason && xr_strlen(Reason))
	{
		ENetPacket* p = enet_packet_create(Reason, xr_strlen(Reason) + 1, ENET_PACKET_FLAG_RELIABLE);
		if (p && enet_peer_send(peer, xr_enet::chan_handshake, p) < 0)
			enet_packet_destroy(p);
		enet_host_flush(m_enet_host);
	}

	enet_peer_disconnect(peer, 0);
	return true;
}

//------------------------------------------------------------------------------
bool IPureServer::GetClientAddress(ClientID ID, ip_address& Address, DWORD* pPort)
{
	ENetPeer* peer = peer_of(ID);
	if (!peer)
		return false;

	Address.m_data.data = peer->address.host;
	if (pPort)
		*pPort = peer->address.port;
	return true;
}

#endif // XR_USE_ENET
