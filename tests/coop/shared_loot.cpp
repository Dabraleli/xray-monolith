
#include <map>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstdio>
using u16=unsigned short; using u32=unsigned; using u8=unsigned char;
constexpr int GE_TRADE_SELL=1,GE_TRADE_BUY=2,M_EVENT_PACK=3,TRUE=1,BroadcastCID=0;
struct {u32 dwTimeGlobal=100;} Device;
struct CSE_Abstract {virtual ~CSE_Abstract(){} u16 ID=0,ID_Parent=65535; bool reachable=true; std::vector<u16> children;};
struct CSE_ALifeCreatureActor:CSE_Abstract {bool alive=true; bool g_Alive(){return alive;}};
struct CSE_ALifeInventoryItem:CSE_Abstract {};
template<class T> T smart_cast(CSE_Abstract* x){return dynamic_cast<T>(x);}
struct NET_Packet {struct {u32 count=20;char data[20];}B;u16 item=0;u32 r_tell(){return 0;}u16 r_u16(){return item;}
void w_begin(int){} void w_u8(u8){} void w(const void*,u32){} };
struct Client {CSE_Abstract* owner;std::map<u16,std::pair<u16,u32>> coop_transfers;};
struct game_sv_Coop {static inline int rejects=0;static void SendLua(u16,const char*){++rejects;}};
bool coop_lootable_container(CSE_Abstract* b,CSE_Abstract* c){return c && (c==b || (c->reachable && !dynamic_cast<CSE_ALifeCreatureActor*>(c)));}
int net_flags(int,int){return 0;}
struct Server {
std::map<u16,CSE_Abstract*> objects;int broadcasts=0;
CSE_Abstract* ID_to_entity(u16 id){auto it=objects.find(id);return it==objects.end()?nullptr:it->second;}
void Perform_transfer(NET_Packet&,NET_Packet&,CSE_Abstract* item,CSE_Abstract* from,CSE_Abstract* to){
 assert(item->ID_Parent==from->ID);auto i=std::find(from->children.begin(),from->children.end(),item->ID);assert(i!=from->children.end());
 from->children.erase(i);to->children.push_back(item->ID);item->ID_Parent=to->ID;
}
void SendBroadcast(int,NET_Packet&,int){++broadcasts;}
u32 event(Client* CL,int event_type,u16 destination,u16 id){ NET_Packet P;P.item=id;
            // A container move is a pair in the legacy client protocol. Keep the
            // source attached until both halves arrive, then commit and broadcast once.
            if (event_type == GE_TRADE_SELL || event_type == GE_TRADE_BUY)
            {
                if (P.B.count < P.r_tell() + sizeof(u16)) return 0;
                const u16 item_id = P.r_u16();
                const u32 now = Device.dwTimeGlobal;
                for (auto it = CL->coop_transfers.begin(); it != CL->coop_transfers.end();)
                    if (now - it->second.second > 5000) it = CL->coop_transfers.erase(it);
                    else ++it;
                CSE_Abstract* body = CL->owner;
                CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(body);
                CSE_Abstract* item = ID_to_entity(item_id);
                CSE_Abstract* target = ID_to_entity(destination);
                if (!actor || !actor->g_Alive() || !item || !smart_cast<CSE_ALifeInventoryItem*>(item)) return 0;
                if (event_type == GE_TRADE_SELL)
                {
                    CL->coop_transfers.erase(item_id);
                    if (item->ID_Parent == destination && coop_lootable_container(body, target))
                        CL->coop_transfers[item_id] = std::make_pair(destination, now);
                    return 0;
                }
                auto reject_move = [&]() -> u32
                {
                    game_sv_Coop::SendLua(body->ID, "loot_refresh|rejected");
                    return 0;
                };
                auto request = CL->coop_transfers.find(item_id);
                if (request == CL->coop_transfers.end()) return reject_move();
                const u16 source_id = request->second.first;
                CL->coop_transfers.erase(request);
                CSE_Abstract* source = ID_to_entity(source_id);
                // Both endpoints are checked again. One must be the requesting body.
                if (source_id == destination || item->ID_Parent != source_id ||
                    (source != body && target != body) || !coop_lootable_container(body, source) ||
                    !coop_lootable_container(body, target)) return reject_move();
                if (std::find(source->children.begin(), source->children.end(), item_id) == source->children.end()) return reject_move();
                NET_Packet reject, take;
                Perform_transfer(reject, take, item, source, target);
                NET_Packet pack;
                pack.w_begin(M_EVENT_PACK);
                pack.w_u8(u8(reject.B.count)); pack.w(reject.B.data, reject.B.count);
                pack.w_u8(u8(take.B.count)); pack.w(take.B.data, take.B.count);
                SendBroadcast(BroadcastCID, pack, net_flags(TRUE, TRUE));
                return 0;
            }

return 0;}
};
int main(){Server s;CSE_ALifeCreatureActor a,b;CSE_Abstract box;CSE_ALifeInventoryItem x,y,z;
a.ID=1;b.ID=2;box.ID=3;x.ID=4;y.ID=5;z.ID=6;
for(CSE_Abstract* o:std::vector<CSE_Abstract*>{&a,&b,&box,&x,&y,&z})s.objects[o->ID]=o;
x.ID_Parent=y.ID_Parent=z.ID_Parent=3;box.children={4,5,6};Client ca{&a},cb{&b};
s.event(&ca,1,3,4);s.event(&cb,1,3,4);assert(x.ID_Parent==3 && s.broadcasts==0);
s.event(&ca,2,1,4);s.event(&cb,2,2,4);assert(x.ID_Parent==1 && s.broadcasts==1 && game_sv_Coop::rejects==1);
s.event(&ca,2,1,4);assert(s.broadcasts==1);
// Multiple Shift/take-all sources queued before their second halves.
s.event(&ca,1,3,5);s.event(&ca,1,3,6);s.event(&ca,2,1,6);s.event(&ca,2,1,5);
assert(y.ID_Parent==1 && z.ID_Parent==1 && s.broadcasts==3);
// Interrupted intent leaves item in source; expired buy cannot move it.
s.event(&ca,1,1,5);Device.dwTimeGlobal+=5001;s.event(&ca,2,3,5);assert(y.ID_Parent==1);
// Distance rechecked at commit.
s.event(&ca,1,1,5);box.reachable=false;s.event(&ca,2,3,5);assert(y.ID_Parent==1);box.reachable=true;
// Forged buy / another player's inventory is rejected.
s.event(&cb,2,2,5);s.event(&cb,1,1,5);s.event(&cb,2,2,5);assert(y.ID_Parent==1);
// Item destroyed or moved between halves never transfers.
s.event(&ca,1,1,6);s.objects.erase(6);s.event(&ca,2,3,6);assert(s.broadcasts==3);
puts("PASS atomic shared loot: competing players, repeated packets, batch transfers, expiry, reach, ownership and destruction");
}
