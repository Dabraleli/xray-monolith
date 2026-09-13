#pragma once
class CSE_Abstract;

// The ALife object whose Lua runs on the coop server (scheduled update, switch checks): "the
// actor" it asks about is the player nearest to it (game_sv_Coop::WorldActorAnchor). Nestable.
struct CoopSpatialScope
{
	const CSE_Abstract* saved;
	CoopSpatialScope(const CSE_Abstract* entity);
	~CoopSpatialScope();
};
