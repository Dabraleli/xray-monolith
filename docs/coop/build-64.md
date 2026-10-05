# Coop build 64

Checkpoint: 2026-10-05. Runtime uses the build 62 engine and build 64 coop
scripts. Update the server and all clients together. Runtime `.script`
files in `gamedata` use Windows-1251 encoding.

## Current behavior

- NPC scheduling uses nearby player bodies. Monster attacks, ammunition
  counts and moving door elements include the pending synchronization fixes.
- IDs in the server's `[personal_infos]` belong to individual players.
  Personal state survives reconnects and respawns; historical shared values
  are not assigned to every player.
- `[start]` chooses the first spawn and respawn location on each level.
  Existing saved bodies keep their positions. Invalid positions fall back
  to the default location. The example places new bodies inside Cordon's
  southern gates; a small offset separates players.
- Sakharov's psi-helmet order, payment, timer, receipt and PDA entry are
  personal. Original modpack functions retain prices and item creation;
  world objectives remain shared. Legacy shared pending orders require an
  identified payer before migration. Player identity follows the existing
  character-name system.
- The quickload binding requests the newest server quicksave. After joining,
  the normal load dialog lists server saves and modification dates. A
  confirmed selection reloads the shared world and reconnects all players.
  Before joining, the dialog retains its local-save behavior. Remote save
  deletion and screenshot transfer are not provided.

## Verification

The engine compiled successfully. Local runs with a real server and two
clients checked personal flags, save restoration, respawn, psi-helmet
payments and receipts, and both server-loading paths. Quickload restored
40000 money; loading a selected manual save restored 60000. Both clients
reconnected and had no local `.scop` files. Lua tests additionally covered
separate order timers, duplicate callbacks, invalid load requests, missing
saves and stale menu responses.

The southern gate spawn was checked in a real client and game screenshots.
Save-menu behavior was exercised through its actual methods; final visual
layout was not inspected. Full GAMMA manual acceptance and LAN testing on
separate computers remain separate checks.

## Next work

The first player's gameplay difficulty, economy difficulty and Open Zone
choice are not yet transferred to initialize a new server world. Existing
saves must keep their own settings when this feature is implemented.
