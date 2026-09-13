#pragma once

// Coop per-player store (M_COOP_PLAYER_STORE): the Lua state of a player's client as text, kept by
// the server under the connection name and handed back on the next join. A modded client's state
// (the alife_storage_manager table, see coop_client_actor.store_collect) does not fit a NET_Packet,
// so a blob travels in parts: each carries the total size, its offset and its bytes; the receiver
// appends them in order (the message is reliable and sequenced) and commits the whole at the end.
const u32 coop_store_limit = 256 * 1024;
const u32 coop_store_part = 8 * 1024;

IC void coop_store_write_part(NET_Packet& P, LPCSTR blob, u32 total, u32 offset, u32 length)
{
	P.w_u32(total);
	P.w_u32(offset);
	P.w_u16(u16(length));
	P.w(blob + offset, length);
}

// One per sender: collects the parts of the blob in flight.
class CCoopStoreAssembler
{
	xr_string m_buffer;
	u32 m_total;

public:
	CCoopStoreAssembler() : m_total(0) {}
	void reset()
	{
		m_buffer.clear();
		m_total = 0;
	}
	// true once the last part arrived: blob holds the whole. A part out of order or over the
	// limits drops the blob in flight; the sender's next blob starts over at offset 0.
	bool receive(NET_Packet& P, xr_string& blob)
	{
		if (P.r_elapsed() < sizeof(u32) * 2 + sizeof(u16)) return false;
		const u32 total = P.r_u32();
		const u32 offset = P.r_u32();
		const u32 length = P.r_u16();
		if (!total || total > coop_store_limit || length > coop_store_part || offset + length > total ||
			P.r_elapsed() < length)
		{
			reset();
			return false;
		}
		if (offset == 0)
		{
			m_buffer.clear();
			m_total = total;
		}
		else if (total != m_total || offset != m_buffer.size())
		{
			reset();
			return false;
		}
		m_buffer.append((LPCSTR)(P.B.data + P.r_tell()), length);
		P.r_advance(length);
		if (m_buffer.size() < total) return false;
		blob.swap(m_buffer);
		reset();
		return true;
	}
};
