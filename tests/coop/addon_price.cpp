#include <cassert>
#include <cstdio>
using u8=unsigned char;
struct CEatableItem { int maximum,remaining; bool disposable;
 int GetMaxUses(){return maximum;} int GetRemainingUses(){return remaining;}
 bool CanDelete(){return disposable;} };
unsigned price(CEatableItem* item,unsigned result){
	CEatableItem* eatable_item = item;
	// Persistent single-use addons use the eat action to install, but their
	// use counter is not a remaining charge or a measure of their value.
	if (eatable_item && eatable_item->GetMaxUses() &&
		(eatable_item->CanDelete() || eatable_item->GetMaxUses() > 1))
	{
		u8 max_uses = eatable_item->GetMaxUses();
		u8 remaining_uses = eatable_item->GetRemainingUses();
		result = result * remaining_uses / max_uses;
		if (result < 1) result = 1;
	}

return result;}
int main(){
 CEatableItem kit{1,0,false},fullkit{1,1,false},tool{4,2,false},food{1,0,true},drink{4,1,true};
 assert(price(&kit,14200)==14200); assert(price(&fullkit,14200)==14200);
 assert(price(&tool,1000)==500); assert(price(&food,200)==1);
 assert(price(&drink,400)==100); assert(price(nullptr,500)==500);
 puts("PASS kit price independent of use action; multiuse tools and consumables retain scaling");
}
