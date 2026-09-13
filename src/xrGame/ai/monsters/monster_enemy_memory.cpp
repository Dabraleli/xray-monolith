#include "pch_script.h"
#include "monster_enemy_memory.h"
#include "BaseMonster/base_monster.h"
#include "../../memory_manager.h"
#include "../../visual_memory_manager.h"
#include "../../enemy_manager.h"
#include "../../ai_object_location.h"
#include "monster_home.h"
#include "Dog/dog.h"
#include "ai_monster_squad.h"
#include "ai_monster_squad_manager.h"
#include "../../Actor.h"
#include "../../actor_memory.h"
#include "../../level.h"
#include "../../xrServer.h"
#include "xrServer_Objects.h"

CMonsterEnemyMemory::CMonsterEnemyMemory()
{
	monster = 0;
	time_memory = 15000;
}

CMonsterEnemyMemory::~CMonsterEnemyMemory()
{
}

void CMonsterEnemyMemory::init_external(CBaseMonster* M, TTime mem_time)
{
	monster = M;
	time_memory = mem_time;
}

extern CActor* g_actor;

void CMonsterEnemyMemory::update()
{
	VERIFY(monster->g_Alive());

	CMonsterHitMemory& monster_hit_memory = monster->HitMemory;

	typedef CObjectManager<const CEntityAlive>::OBJECTS objects_list;

	objects_list const& objects = monster->memory().enemy().objects();

	if (monster_hit_memory.is_hit() && time() < monster_hit_memory.get_last_hit_time() + 1000)
	{
		if (CEntityAlive* enemy = smart_cast<CEntityAlive*>(monster->HitMemory.get_last_hit_object()))
		{
			if (monster->CCustomMonster::useful(&monster->memory().enemy(), enemy) &&
				monster->Position().distance_to(enemy->Position())
				<
				monster->get_feel_enemy_who_just_hit_max_distance())
			{
				add_enemy(enemy);

				bool const self_is_dog = !!smart_cast<const CAI_Dog*>(monster);
				if (self_is_dog)
				{
					CMonsterSquad* const squad = monster_squad().get_squad(monster);
					squad->set_home_in_danger();
				}
			}
		}
	}

	if (monster->SoundMemory.IsRememberSound() && g_actor
		&& g_actor->memory().visual().visible_now(monster))
	{
		SoundElem sound;
		bool dangerous;
		monster->SoundMemory.GetSound(sound, dangerous);
		if (dangerous && Device.dwTimeGlobal < sound.time + 2000)
		{
			if (CEntityAlive const* enemy = smart_cast<CEntityAlive const*>(sound.who))
			{
				float const xz_dist = monster->Position().distance_to_xz(enemy->Position());
				float const y_dist = _abs(monster->Position().y - enemy->Position().y);

				if (monster->CCustomMonster::useful(&monster->memory().enemy(), enemy) &&
					y_dist < 10 &&
					xz_dist < monster->get_feel_enemy_who_made_sound_max_distance())
				{
					add_enemy(enemy);

					bool const self_is_dog = !!smart_cast<const CAI_Dog*>(monster);
					if (self_is_dog)
					{
						CMonsterSquad* const squad = monster_squad().get_squad(monster);
						squad->set_home_in_danger();
					}
				}
			}
		}
	}

	for (objects_list::const_iterator I = objects.begin();
	     I != objects.end();
	     ++I)
	{
		const CEntityAlive* enemy = *I;
		const bool feel_enemy = monster->Position().distance_to(enemy->Position())
			<
			monster->get_feel_enemy_max_distance();

		if (feel_enemy || monster->memory().visual().visible_now(*I))
			add_enemy(*I);
	}

    if (strstr(Core.Params,"-coop_damage_probe")) {
        // Diagnostic only: every monster reports why it does or does not see nearby players.
        static xr_map<u16,u32> reports; u32& last=reports[monster->ID()];
        if (Device.dwTimeGlobal-last>=1000) {
            last=Device.dwTimeGlobal;
            for (u32 n=0;n<Level().Objects.o_count();++n) {
                CActor* actor=smart_cast<CActor*>(Level().Objects.o_get_by_iterator(n));
                if (!actor || actor->ID()==0 || monster->Position().distance_to(actor->Position())>=15.f) continue;
                float pending=0.f;
                for (const auto& item : monster->memory().visual().not_yet_visible_objects())
                    if (item.m_object==actor) pending=item.m_value;
                Msg("[COOP_ENEMY] monster=%u section=%s actor=%u monster_team=%u actor_team=%u relation=%u useful=%u visible=%u see_now=%u lum=%f pending=%f threshold=%f melee_dist=%f melee_min=%f distance=%f enemy=%u",
                    monster->ID(),*monster->cNameSect(),actor->ID(),monster->g_Team(),actor->g_Team(),u32(monster->tfGetRelationType(actor)),
                    monster->memory().enemy().is_useful(actor),monster->memory().visual().visible_now(actor),monster->memory().visual().visible_right_now(actor),
                    monster->memory().visual().object_luminocity(actor),pending,monster->memory().visual().current_state().m_visibility_threshold,
                    monster->MeleeChecker.distance_to_enemy(actor),monster->MeleeChecker.get_min_distance(),
                    monster->Position().distance_to(actor->Position()),monster->EnemyMan.get_enemy()?monster->EnemyMan.get_enemy()->ID():u16(-1));
            }
        }
    }
	float const feel_enemy_max_distance = monster->get_feel_enemy_max_distance();
    if (IsGameTypeCoop() && OnServer() && Level().Server)
    {
        xr_vector<u16> players;
        auto collect = [&](IClient* connection) {
            if (connection==Level().Server->GetServerClient() || !connection->flags.bConnected) return;
            xrClientData* client=static_cast<xrClientData*>(connection);
            if (client->owner) players.push_back(client->owner->ID);
        };
        Level().Server->ForEachClientDo(collect);
        for (u16 id : players) {
            CActor* actor=smart_cast<CActor*>(Level().Objects.net_Find(id));
            if (!actor || !actor->g_Alive() || !monster->memory().enemy().is_useful(actor)) continue;
            if (monster->Position().distance_to_xz(actor->Position())>=feel_enemy_max_distance ||
                _abs(monster->Position().y-actor->Position().y)>=10.f) continue;
            Fvector from,to,direction;
            monster->Center(from); actor->Center(to); direction.sub(to,from);
            const float distance=direction.magnitude();
            if (distance>EPS) {
                direction.div(distance);
                collide::rq_result result;
                if (Level().ObjectSpace.RayPick(from,direction,distance,collide::rqtBoth,result,monster) && result.O!=actor) continue;
            }
            add_enemy(actor);
        }
    }
    else if (g_actor)
	{
		float const xz_dist = monster->Position().distance_to_xz(g_actor->Position());
		float const y_dist = _abs(monster->Position().y - g_actor->Position().y);

		if (xz_dist < feel_enemy_max_distance &&
			y_dist < 10 &&
			monster->memory().enemy().is_useful(g_actor) &&
			g_actor->memory().visual().visible_now(monster))
		{
			add_enemy(g_actor);
		}
	}

	// удалить устаревших врагов
	remove_non_actual();

	// обновить опасность 
	for (ENEMIES_MAP_IT it = m_objects.begin(); it != m_objects.end(); it++)
	{
		u8 relation_value = u8(monster->tfGetRelationType(it->first));
		float dist = monster->Position().distance_to(it->second.position);
		it->second.danger = (1 + relation_value * relation_value * relation_value) / (1 + dist);
	}
}

void CMonsterEnemyMemory::add_enemy(const CEntityAlive* enemy)
{
	SMonsterEnemy enemy_info;
	enemy_info.position = enemy->Position();
	enemy_info.vertex = enemy->ai_location().level_vertex_id();
	enemy_info.time = Device.dwTimeGlobal;
	enemy_info.danger = 0.f;

	ENEMIES_MAP_IT it = m_objects.find(enemy);
	if (it != m_objects.end())
	{
		// обновить данные о враге
		it->second = enemy_info;
	}
	else
	{
		// добавить врага в список объектов
		m_objects.insert(mk_pair(enemy, enemy_info));
	}
}

void CMonsterEnemyMemory::add_enemy(const CEntityAlive* enemy, const Fvector& pos, u32 vertex, u32 time)
{
	SMonsterEnemy enemy_info;
	enemy_info.position = pos;
	enemy_info.vertex = vertex;
	enemy_info.time = time;
	enemy_info.danger = 0.f;

	ENEMIES_MAP_IT it = m_objects.find(enemy);
	if (it != m_objects.end())
	{
		// обновить данные о враге
		if (it->second.time < enemy_info.time) it->second = enemy_info;
	}
	else
	{
		// добавить врага в список объектов
		m_objects.insert(mk_pair(enemy, enemy_info));
	}
}

void CMonsterEnemyMemory::remove_non_actual()
{
	TTime cur_time = Device.dwTimeGlobal;

	// удалить 'старых' врагов и тех, расстояние до которых > 30м и др.
	for (ENEMIES_MAP_IT it = m_objects.begin(), nit;
	     it != m_objects.end();
	     it = nit)
	{
		nit = it;
		++nit;
		// проверить условия удаления
		if (!it->first ||
			!it->first->g_Alive() ||
			it->first->getDestroy() ||
			(it->second.time + time_memory < cur_time) ||
			(it->first->g_Team() == monster->g_Team()) ||
			!monster->memory().enemy().is_useful(it->first))
		{
			m_objects.erase(it);
		}
	}
}

const CEntityAlive* CMonsterEnemyMemory::get_enemy()
{
	ENEMIES_MAP_IT it = find_best_enemy();
	if (it != m_objects.end()) return it->first;
	return (0);
}

SMonsterEnemy CMonsterEnemyMemory::get_enemy_info()
{
	SMonsterEnemy ret_val;
	ret_val.time = 0;

	ENEMIES_MAP_IT it = find_best_enemy();
	if (it != m_objects.end()) ret_val = it->second;

	return ret_val;
}

ENEMIES_MAP_IT CMonsterEnemyMemory::find_best_enemy()
{
	ENEMIES_MAP_IT it = m_objects.end();
	float max_value = 0.f;

	// find best at home first
	for (ENEMIES_MAP_IT I = m_objects.begin(); I != m_objects.end(); I++)
	{
		if (!monster->Home->at_home(I->second.position)) continue;
		if (I->second.danger > max_value)
		{
			max_value = I->second.danger;
			it = I;
		}
	}

	// there is no best enemies at home
	if (it == m_objects.end())
	{
		// find any
		max_value = 0.f;
		for (ENEMIES_MAP_IT I = m_objects.begin(); I != m_objects.end(); I++)
		{
			if (I->second.danger > max_value)
			{
				max_value = I->second.danger;
				it = I;
			}
		}
	}

	return it;
}

void CMonsterEnemyMemory::remove_links(CObject* O)
{
	if (monster)
	{
		monster->EnemyMan.remove_links(O);
	}

	for (ENEMIES_MAP_IT I = m_objects.begin(); I != m_objects.end(); ++I)
	{
		if ((*I).first == O)
		{
			m_objects.erase(I);
			break;
		}
	}
}
