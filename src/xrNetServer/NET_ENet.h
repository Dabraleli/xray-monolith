#pragma once
// ENet-backed transport for xrNetServer.
//
// Enabled by defining XR_USE_ENET for the xrNetServer, xrGame and xrEngine
// projects. When it is on, the DirectPlay 8 headers are not included at all and
// this header supplies the handful of DPNSEND_* constants that the engine still
// uses as its own send-flag vocabulary (see net_flags() in NET_Messages.h).
//
// The DirectPlay implementation is left in the tree untouched and simply
// compiled out, so upstream merges keep working.

#ifdef XR_USE_ENET

#include <enet/enet.h>

// This header is pulled in from xrGame's PCH as well, where the xrNetServer
// export macro has not been introduced yet -- it expands to nothing in every
// configuration, so define it here if we got there first.
#ifndef XRNETSERVER_API
#define XRNETSERVER_API
#endif

// ---------------------------------------------------------------------------
// Send flags.
//
// These keep their original DirectPlay names and values so that no call site
// has to change: net_flags() still builds the same bit mask, we just interpret
// it ourselves in the transport.
// ---------------------------------------------------------------------------
#ifndef DPNSEND_GUARANTEED
#define DPNSEND_GUARANTEED 0x0001
#define DPNSEND_NOCOMPLETE 0x0002
#define DPNSEND_NONSEQUENTIAL 0x0004
#define DPNSEND_PRIORITY_HIGH 0x0008
#define DPNSEND_PRIORITY_LOW 0x0010
#define DPNSEND_NOLOOPBACK 0x0020
#define DPNSEND_NOCOPY 0x0040
#endif

// The engine's own flag, already defined in NET_Messages.h, repeated here only
// for readability of the mapping below: 0x0100 == DPNSEND_IMMEDIATELLY.

// ---------------------------------------------------------------------------
// Drop-in for the DirectPlay connection statistics block. Only the fields the
// engine actually reads are kept (see IClientStatistic in NET_Shared.h and
// IClientStatistic::Update in NET_Server.cpp); they are filled from ENet's own
// per-peer counters.
// ---------------------------------------------------------------------------
struct DPN_CONNECTION_INFO
{
	u32 dwSize;
	u32 dwRoundTripLatencyMS;
	u32 dwThroughputBPS;
	u32 dwPeakThroughputBPS;
	u32 dwPacketsDropped;
	u32 dwPacketsRetried;
	u32 dwMessagesReceived;
	u32 dwMessagesTransmittedHighPriority;
	u32 dwMessagesTransmittedNormalPriority;
	u32 dwMessagesTransmittedLowPriority;
};

namespace xr_enet
{
	// Fill the statistics block above from a live ENet peer.
	XRNETSERVER_API void fill_connection_info(const ENetPeer* peer, DPN_CONNECTION_INFO& ci);

	// Channel layout. ENet guarantees ordering per channel, so the two
	// priorities travel independently and a flood of low-priority world
	// updates cannot delay a high-priority one.
	enum
	{
		chan_normal = 0,
		chan_high = 1,
		chan_handshake = 2, // SClientConnectData, which DirectPlay carried in player info
		chan_count = 3
	};

	IC enet_uint8 channel_of(u32 xr_flags)
	{
		return (xr_flags & DPNSEND_PRIORITY_HIGH) ? chan_high : chan_normal;
	}

	IC enet_uint32 packet_flags_of(u32 xr_flags)
	{
		if (xr_flags & DPNSEND_GUARANTEED)
			return ENET_PACKET_FLAG_RELIABLE;

		// Unreliable. DirectPlay's "non sequential" means the receiver may see
		// packets out of order; ENet expresses that as UNSEQUENCED, which also
		// opts out of the channel's sequence numbering.
		if (xr_flags & DPNSEND_NONSEQUENTIAL)
			return ENET_PACKET_FLAG_UNSEQUENCED;

		return 0; // unreliable but sequenced
	}

	// ENet peers are identified by their slot in the host's peer array, which
	// is stable for the lifetime of the connection but starts at 0 -- and a
	// ClientID of 0 is treated as "invalid" throughout the engine. Shift by one.
	IC u32 client_id_of(const ENetPeer* peer) { return u32(peer->incomingPeerID) + 1; }

	IC bool is_local_address(enet_uint32 host)
	{
		return (host & 0x000000FF) == 127; // 127.0.0.0/8, network byte order
	}

	// One-time enet_initialize() with matching shutdown at process exit.
	XRNETSERVER_API bool initialize();

	// A listen server shares its process with one of its own clients. DirectPlay
	// ran its own threads, ENet does not: while that client sits in a blocking
	// wait, nothing drives the server's host and it never answers. The server
	// publishes a pump here, and every blocking wait in the client calls it.
	typedef void (*pump_proc)();
	XRNETSERVER_API void set_local_pump(pump_proc pump);
	XRNETSERVER_API void pump_local();
} // namespace xr_enet

#endif // XR_USE_ENET
