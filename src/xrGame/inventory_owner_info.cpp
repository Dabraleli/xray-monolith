//////////////////////////////////////////////////////////////////////
// inventory_owner_info.h:	для работы с сюжетной информацией
//
//////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "InventoryOwner.h"
#include "GameObject.h"
#include "xrMessages.h"
#include "ai_space.h"
#include "ai_debug.h"
#include "alife_simulator.h"
#include "alife_registry_container.h"
#include "script_game_object.h"
#include "level.h"
#include "infoportion.h"
#include "alife_registry_wrappers.h"
#include "script_callback_ex.h"
#include "game_object_space.h"

void CInventoryOwner::OnEvent(NET_Packet& P, u16 type)
{
	switch (type)
	{
	case GE_INFO_TRANSFER:
		{
			u16 id;
			shared_str info_id;
			u8 add_info;

			P.r_u16(id); //отправитель
			P.r_stringZ(info_id); //номер полученной информации
			P.r_u8(add_info); //добавление или убирание информации

			if (add_info)
				OnReceiveInfo(info_id);
			else
				OnDisableInfo(info_id);
		}
		break;
	}
}


// ---- Coop: personal portions ------------------------------------------------------------------
// coop_server.ltx [personal_infos] (the server's own file): one portion id per line. Read at the first
// question; an edited list applies from the next server start.
bool CInventoryOwner::coop_personal_info(const shared_str& info_id)
{
	static xr_vector<shared_str> listed;
	static bool loaded = false;
	if (!loaded && IsGameTypeCoop() && OnServer())
	{
		loaded = true;
		string_path config_path;
		FS.update_path(config_path, "$app_data_root$", "coop_server.ltx");
		if (FS.exist(config_path))
		{
			CInifile config(config_path);
			if (config.section_exist("personal_infos"))
			{
				LPCSTR name, value;
				for (u32 i = 0; config.r_line("personal_infos", i, &name, &value); ++i)
					if (name && xr_strlen(name)) listed.push_back(shared_str(name));
			}
		}
		xr_string text;
		for (u32 i = 0; i < listed.size(); ++i)
		{
			text += i ? ", " : " ";
			text += listed[i].c_str();
		}
		Msg("[COOP_SERVER] PERSONAL_INFOS count=%u%s", u32(listed.size()), text.c_str());
	}
	if (listed.empty() || !info_id.size()) return false;
	return std::find(listed.begin(), listed.end(), info_id) != listed.end();
}

void CInventoryOwner::coop_personal_infos(u16 holder, xr_vector<shared_str>& out)
{
	out.clear();
	if (holder == u16(-1) || !ai().get_alife()) return;
	const KNOWN_INFO_VECTOR* known = ai().alife().registry((CInfoPortionRegistry*)NULL).object(holder, true);
	if (!known) return;
	for (u32 i = 0; i < known->size(); ++i)
		if (coop_personal_info((*known)[i])) out.push_back((*known)[i]);
}

void CInventoryOwner::coop_copy_personal_infos(u16 from, u16 to)
{
	xr_vector<shared_str> infos;
	coop_personal_infos(from, infos);
	if (infos.empty() || to == u16(-1)) return;
	CInfoPortionRegistry& registry = ai().alife().registry((CInfoPortionRegistry*)NULL);
	KNOWN_INFO_VECTOR* known = registry.object(to, true);
	if (!known)
	{
		KNOWN_INFO_VECTOR fresh;
		registry.add(to, fresh, false);
		known = registry.object(to, true);
	}
	if (!known) return;
	for (u32 i = 0; i < infos.size(); ++i)
		if (std::find_if(known->begin(), known->end(), CFindByIDPred(infos[i])) == known->end()) known->push_back(infos[i]);
	Msg("[COOP_SERVER] PERSONAL_INFO_COPY from=%u to=%u infos=%u", from, to, u32(infos.size()));
}

bool CInventoryOwner::OnReceiveInfo(shared_str info_id) const
{
	VERIFY(info_id.size());
	//добавить запись в реестр
	// Coop server: a [personal_infos] portion goes to the body's own registry, not the shared book.
	const bool personal = coop_personal(info_id);
	KNOWN_INFO_VECTOR& known_info = personal ? m_known_info_registry->registry().objects(m_coop_personal_holder)
	                                         : m_known_info_registry->registry().objects();
	KNOWN_INFO_VECTOR_IT it = std::find_if(known_info.begin(), known_info.end(), CFindByIDPred(info_id));
	if (known_info.end() == it)
		known_info.push_back(/*INFO_DATA(*/info_id/*, Level().GetGameTime())*/);
	else
		return false;
	if (personal) Msg("[COOP_SERVER] PERSONAL_INFO body=%u info=%s add=1", m_coop_personal_holder, info_id.c_str());

#ifdef DEBUG
	if(psAI_Flags.test(aiInfoPortion))
		Msg("[%s] Received Info [%s]", Name(), *info_id);
#endif

	return true;
}
#ifdef DEBUG
void CInventoryOwner::DumpInfo() const
{
	KNOWN_INFO_VECTOR& known_info = m_known_info_registry->registry().objects();

	Msg("------------------------------------------");	
	Msg("Start KnownInfo dump for [%s]",Name());	
	KNOWN_INFO_VECTOR_IT it = known_info.begin();
	for(int i=0;it!=known_info.end();++it,++i){
		Msg("known info[%d]:%s",i,(*it).c_str());	
	}
	Msg("------------------------------------------");	

}
#endif

void CInventoryOwner::OnDisableInfo(shared_str info_id) const
{
	VERIFY(info_id.size());
	//удалить запись из реестра

#ifdef DEBUG
	if(psAI_Flags.test(aiInfoPortion))
		Msg("[%s] Disabled Info [%s]", Name(), info_id.c_str());
#endif

	const bool personal = coop_personal(info_id);
	KNOWN_INFO_VECTOR& known_info = personal ? m_known_info_registry->registry().objects(m_coop_personal_holder)
	                                         : m_known_info_registry->registry().objects();

	KNOWN_INFO_VECTOR_IT it = std::find_if(known_info.begin(), known_info.end(), CFindByIDPred(info_id));
	if (known_info.end() == it) return;
	known_info.erase(it);
	if (personal) Msg("[COOP_SERVER] PERSONAL_INFO body=%u info=%s add=0", m_coop_personal_holder, info_id.c_str());
}

void CInventoryOwner::TransferInfo(shared_str info_id, bool add_info) const
{
	VERIFY(info_id.size());
	const CObject* pThisObject = smart_cast<const CObject*>(this);
	VERIFY(pThisObject);

	//отправляем от нашему PDA пакет информации с номером
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_INFO_TRANSFER, pThisObject->ID());
	P.w_u16(pThisObject->ID()); //отправитель
	P.w_stringZ(info_id); //сообщение
	P.w_u8(add_info ? 1 : 0); //добавить/удалить информацию
	CGameObject::u_EventSend(P);

	CInfoPortion info_portion;
	info_portion.Load(info_id);
	{
		if (add_info)
			OnReceiveInfo(info_id);
		else
			OnDisableInfo(info_id);
	}
}

bool CInventoryOwner::HasInfo(shared_str info_id) const
{
	VERIFY(info_id.size());
	const KNOWN_INFO_VECTOR* known_info = coop_personal(info_id) ? m_known_info_registry->registry().objects_ptr(m_coop_personal_holder)
	                                                             : m_known_info_registry->registry().objects_ptr();
	if (!known_info) return false;

	if (std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(info_id)) == known_info->end())
		return false;

	return true;
}

/*
bool CInventoryOwner::GetInfo	(shared_str info_id, INFO_DATA& info_data) const
{
	VERIFY( info_id.size() );
	const KNOWN_INFO_VECTOR* known_info = m_known_info_registry->registry().objects_ptr ();
	if(!known_info) return false;

	KNOWN_INFO_VECTOR::const_iterator it = std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(info_id));
	if(known_info->end() == it)
		return false;

	info_data = *it;
	return true;
}
*/
