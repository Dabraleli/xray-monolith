from pathlib import Path
import subprocess, tempfile, os

root=Path(__file__).resolve().parents[2]
ev=Path(tempfile.mkdtemp(prefix='coop-loot-test-'))
scripts=root/'gamedata/scripts'
test=r'''
local now=1000
function tostring(x)if x==nil then return 'nil' elseif x==true then return 'true' elseif x==false then return 'false' end return ''..x end
string.format=function(fmt,...)
 local a={...}
 return 'loot_action|'..tostring(a[1])..'|'..tostring(a[2])..'|'..tostring(a[3])
end
function time_global() return now end
function printf() end
local objects={}
local function object(id,parent,kind)
 local o={kind=kind,parent_id=parent,id=function()return id end,parent=function()return objects[parent]end,
 alive=function(self)return self.living end,position=function()return {distance_to_sqr=function()return 4 end}end,
 section=function()return kind end}
 objects[id]=o;return o
end
local actor=object(1,nil,'actor');actor.living=true
local other=object(2,nil,'actor');other.living=true
local corpse=object(3,nil,'stalker')
local helmet=object(4,3,'helmet')
local tool=object(5,1,'tool')
local weapon=object(6,3,'weapon')
db={actor=actor}
level={object_by_id=function(id)return objects[id]end,coop_send_lua=function()end}
function alife_object(id)return objects[id]end
function IsInvbox(o)return o.kind=='box'end
function IsStalker(o)return o.kind=='stalker'end
function IsWeapon(o)return o.kind=='weapon'end
function IsOutfit(o)return false end
function IsHeadgear(o)return o.kind=='helmet'end
function GetItemList()return {tool=0.1}end
local data={part=45};local destroyed,degraded,outputs=0,0,0
item_parts={is_suitable_dtool=function()return true end,get_parts_con=function()return data end,
 disassembly_outfit=function(o,t)destroyed=destroyed+1;degraded=degraded+1;outputs=outputs+1 end}
function se_load_var()return data end
magazine_binder={get_data=function()return nil end}
zzzz_arti_jamming_repairs={is_part=function(p)return p=='part'end,act_fieldstrip=function()outputs=outputs+1;data.part=-1 end}
arti_jamming={is_barrel=function()return false end}
local loaded=true
magazines={is_supported_weapon=function()return true end,get_mag_loaded=function()return loaded end,
 eject_magazine=function()loaded=false;outputs=outputs+1 end}
utils_item={detach_addon=function()end}
item_weapon={menu_scope=function()return false end}
'''
test+="\ncoop_loot_lock={}\n"+"local function lock_module()\n"+(scripts/'coop_loot_lock.script').read_text(encoding='utf8')+"\nend\nsetfenv(lock_module,setmetatable(coop_loot_lock,{__index=_G}));lock_module()\n"
test+="\ncoop_loot_actions={}\nlocal function actions_module()\n"+(scripts/'coop_loot_actions.script').read_text(encoding='utf8')+"\nend\nsetfenv(actions_module,setmetatable(coop_loot_actions,{__index=_G}));actions_module()\n"
test+=r'''
local request=coop_loot_actions.server_request
local p={'loot_action','disassemble','4','5'}
assert(not request(actor,p).ok and destroyed==0,'unlocked corpse mutated')
coop_loot_lock.server_request(actor,{'loot_lock','open','3','11'})
assert(not request(other,p).ok and destroyed==0,'other player used lease')
helmet.parent_id=2
assert(not request(actor,p).ok and destroyed==0,'stale local parent authorized dismantling')
helmet.parent_id=3
assert(request(actor,p).ok and destroyed==1 and degraded==1 and outputs==1)
assert(not request(actor,p).ok and destroyed==1 and degraded==1,'duplicate dismantle')
assert(request(actor,{'loot_action','fieldstrip','6','part'}).ok and outputs==2)
assert(not request(actor,{'loot_action','fieldstrip','6','part'}).ok and outputs==2,'duplicate part')
assert(request(actor,{'loot_action','eject','6',''}).ok and outputs==3)
assert(request(actor,{'loot_action','eject','6',''}).ok and outputs==3,'duplicate magazine')
now=7000
assert(not request(actor,{'loot_action','detach','6','sil'}).ok,'expired lease accepted')
io.write('PASS: corpse lease, owner, expiry, duplicate dismantling, parts and magazines\n')
local shared=object(40,nil,'stalker')
coop_loot_lock.server_request(actor,{'loot_lock','open','40','21'})
coop_loot_lock.server_request(other,{'loot_lock','open','40','22'})
assert(coop_loot_lock.server_owns(actor,40) and coop_loot_lock.server_owns(other,40),'second viewer denied')
coop_loot_lock.server_request(actor,{'loot_lock','close','40','21'})
assert(not coop_loot_lock.server_owns(actor,40) and coop_loot_lock.server_owns(other,40),'closing first viewer evicted second')
coop_loot_lock.server_request(other,{'loot_lock','close','40','21'})
assert(coop_loot_lock.server_owns(other,40),'stale token closed session')
local box=object(20,nil,'box');local boxed=object(21,20,'weapon')
assert(not request(actor,{'loot_action','detach','21','sil'}).ok)
coop_loot_actions.server_box_used(actor,box)
assert(request(actor,{'loot_action','detach','21','sil'}).ok)
assert(not request(other,{'loot_action','detach','21','sil'}).ok)
box.position=function()return {distance_to_sqr=function()return 100 end}end
assert(not request(actor,{'loot_action','detach','21','sil'}).ok)
io.write('PASS opened box permits owner action, unopened/other/distant rejected\n')
local clone_calls,released,spawned=0,0,0
local fail_clone=false
local configured={weapon=true,weapon_scope=true,scope=true}
ini_sys={r_string_ex=function()return 'weapon'end,section_exist=function(self,s)return configured[s]end}
function parse_list()return {'scope'}end
utils_data={collect_sections=function()return {scope=true}end}
function alife_object(id)return objects[id]end
function alife_clone_weapon(o,section)
 clone_calls=clone_calls+1
 if fail_clone then return nil end
 return {id=100+clone_calls}
end
function alife_release(o)released=released+1 end
function alife_create_item(section,owner)assert(section=='scope' and owner==actor);spawned=spawned+1 end
local own=object(30,1,'weapon');local addon=object(31,1,'scope')
local scoped=object(32,1,'weapon');scoped.section=function()return 'weapon_scope'end
assert(not request(other,{'loot_action','attach_scope','30','31'}).ok)
fail_clone=true
assert(not request(actor,{'loot_action','attach_scope','30','31'}).ok and released==0)
assert(not request(actor,{'loot_action','detach_scope','32',''}).ok and spawned==0)
fail_clone=false
assert(request(actor,{'loot_action','attach_scope','30','31'}).ok and released==1)
assert(not request(actor,{'loot_action','attach_scope','30','31'}).ok and released==1)
assert(request(actor,{'loot_action','detach_scope','32',''}).ok and spawned==1)
assert(not request(actor,{'loot_action','detach_scope','32',''}).ok and spawned==1)
io.write('PASS: scope conversion, failed clone preserves addon, duplicate requests rejected\n')
local swap=object(33,1,'weapon');swap.section=function()return 'weapon_scope'end
local addon2=object(34,1,'scope2');configured.scope2=true;configured.weapon_scope2=true
parse_list=function()return {'scope','scope2'}end
assert(request(actor,{'loot_action','attach_scope','33','34'}).ok and released==2 and spawned==2)
local magweapon=object(35,1,'weapon')
actor.active_item=function()return magweapon end
magnifier_switch={get_new_section=function(s,state)return state==1 and 'weapon_magnified' or 'weapon'end,
 switch_magnifier=function(w,s,n)return {id=150}end}
assert(not request(actor,{'loot_action','magnifier','35','unrelated_weapon'}).ok)
assert(request(actor,{'loot_action','magnifier','35','weapon_magnified'}).ok)
assert(not request(actor,{'loot_action','magnifier','35','weapon_magnified'}).ok)
ish_toggle_scope={}
haru_quick_action_wheel_mcm={QScopeWheelOption={Attach=function()error('local clone')end,Detach=function()error('local clone')end}}
own.cast_Weapon=function()return {GetScopeName=function()return nil end}end
own.get_state=function()return 0 end
local calls,applied=0,0
actor_effects={play_item_fx=function()end}
actor_menu={set_msg=function()end}
coop_loot_actions.install_client(function(msg)
 calls=calls+1
 assert(type(msg)=='string')
 return {ok=true,parts={part=-1}}
end,function()applied=applied+1 end)
item_parts.disassembly_outfit(helmet,tool)
zzzz_arti_jamming_repairs.act_fieldstrip(6,'part')
magazines.eject_magazine(weapon)
utils_item.detach_addon(weapon,nil,'sil')
assert(calls==4 and applied==4 and destroyed==1 and outputs==3,'client mutated corpse locally')
local before_clones=clone_calls
item_weapon.attach_scope(addon,own)
item_weapon.detach_scope(scoped)
assert(calls==6 and applied==6 and clone_calls==before_clones,'client cloned weapon locally')
ish_toggle_scope.attach_scope_override(addon,own)
haru_quick_action_wheel_mcm.QScopeWheelOption:Attach(addon,own)
haru_quick_action_wheel_mcm.QScopeWheelOption:Detach(own)
magnifier_switch.switch_magnifier(magweapon,nil,'weapon_magnified')
assert(calls==10 and applied==10 and clone_calls==before_clones,'alternative UI cloned locally')
io.write('PASS: four client actions use server without local mutation\n')
local shown=false
local sent={}
actor.is_talking=function()return false end
local baseUIS=function()shown=true end
liz_fdda_redone_body_search={start_body_search=function(obj)return baseUIS('loot',obj)end}
debug={getupvalue=function(f,i)if i==1 then return 'baseUIS',baseUIS end end,
 setupvalue=function(f,i,value)baseUIS=value end}
ui_inventory={UIInventory={ShowDialog=function(self)shown=true end},
 start=function()end}
local gui={mode='loot',npc_id=3,IsShown=function()return shown end,HideDialog=function()shown=false end}
ui_inventory.GUI=gui
coop_loot_lock.install_client(function(msg)sent[#sent+1]=msg end)
ui_inventory.start('loot',corpse)
coop_loot_lock.client_reply({'loot_lock','ok','3','1'})
now=now+1000
coop_loot_lock.client_update()
assert(#sent==2,'FDDA opening must retain lease and ping')
coop_loot_lock.client_reply({'loot_lock','alive','3','1'})
liz_fdda_redone_body_search.start_body_search(corpse)
assert(shown,'delayed FDDA window refused with active lease')
coop_loot_lock.client_update()
shown=false
coop_loot_lock.client_update()
liz_fdda_redone_body_search.start_body_search(corpse)
assert(not shown,'late window opened after lease closed')
io.write('PASS: delayed FDDA opening retains lease; closed lease cannot reopen\n')
local refreshes=0
gui.On_Item_Update=function()refreshes=refreshes+1 end
local function inventory(self,callback)
 for _,item in pairs(objects)do if item:parent()==self then callback(self,item)end end
end
actor.iterate_inventory=inventory;corpse.iterate_inventory=inventory
shown=true
coop_loot_actions.client_update()
now=now+300;coop_loot_actions.client_update()
assert(refreshes==0,'unchanged inventory refreshed')
object(7,3,'part')
now=now+300;coop_loot_actions.client_update()
assert(refreshes==1,'late part spawn did not refresh corpse UI')
now=now+300;coop_loot_actions.client_update()
assert(refreshes==1,'unchanged list repeatedly refreshed')
coop_loot_actions.client_refresh()
coop_loot_actions.client_update()
assert(refreshes==2,'rejected transfer did not refresh stale UI')
io.write('PASS: delayed server spawn refreshes open corpse once\n')
'''
path=ev/'loot-actions.lua';path.write_text(test,encoding='utf8')
run=subprocess.run([os.environ.get('LUA_BIN', str(root/'src/3rd party/luajit-2/src/host/minilua.exe')),str(path)],capture_output=True,text=True)
(ev/'loot-actions.log').write_text(run.stdout+run.stderr,encoding='utf8')
print(run.stdout+run.stderr)
assert run.returncode==0 and run.stdout.count('PASS:')==5 and not run.stderr
