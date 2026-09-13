////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator.h
//	Created 	: 25.12.2002
//  Modified 	: 13.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_interaction_manager.h"
#include "alife_update_manager.h"
#include "script_export_space.h"

#pragma warning(push)
#pragma warning(disable:4005)

class CALifeSimulator :
	public CALifeUpdateManager,
	public CALifeInteractionManager
{
protected:
	virtual void setup_simulator(CSE_ALifeObject* object);
	virtual void reload(LPCSTR section);

public:
	CALifeSimulator(xrServer* server, shared_str* command_line);
	virtual ~CALifeSimulator();
	virtual void destroy();
	IReader const* get_config(shared_str config) const;
    bool uses_player_anchors() const;
    float activation_distance(const Fvector& position, u32 game_vertex_id) const;

#if 0//def DEBUG
			void	validate			();
#endif //DEBUG

private:
	typedef xr_list<std::pair<shared_str, IReader*>> configs_type;
	mutable configs_type m_configs_lru;

DECLARE_SCRIPT_REGISTER_FUNCTION
};

#pragma warning(pop)


#include "alife_simulator_inline.h"
