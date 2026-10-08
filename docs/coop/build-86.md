# Cooperative runtime changes through build 86

Builds 66–85 add magazine lifecycle and ammunition synchronization, injury persistence,
revival fixes, sleep voting, teammate markers, torch/RF support, story-NPC population
repair, trader voice replication, inventory presentation and corpse item actions.

Prepared build 86 adds:
- Living-player quest-carrier validation and hand-in context; repeated PDA jobs reactivate.
- Shared corpse/container searches, per-viewer authorization and atomic remote transfers.
- Server-side scope/kit actions, quick-wheel and magnifier routing; persistent kit pricing.
- Downed client damage guards and server-configured player friendly fire.
- Trader-context accounting, missing-collision-form vision guard and headless HUD bypass.

`server/appdata/coop_server.ltx`, section `[server]`: `friendly_fire = false` disables
player-to-player damage; `true` enables it. Missing means false. Restart the server
process after changing it. Downed immunity remains enabled in either mode.

## Validation and delivery status

The DX11 x64 engine builds. Offline Lua tests and the extracted C++ transaction/pricing
checks passed. Build 86 has NOT been gameplay-tested or deployed; installed delivery
remains build 85. Headless performance improvement is unmeasured. Legacy MP-412 upgrade
and weapon-unjam clone fallbacks have not been changed. Do not treat this checkpoint
as proof that every reported gameplay issue is resolved.

Run `python tests/coop/test_loot.py` with the built minilua executable or set `LUA_BIN`.
It tests the current Lua modules with mocked game services, including two viewers,
ownership transitions, duplicate actions, alternative scope UI and refresh behavior.
`tests/coop/shared_loot.cpp` and `tests/coop/addon_price.cpp` are extracted C++ regression
harnesses; compile with C++17 and assertions enabled. They mock engine services and do
not replace a multiplayer gameplay test.
