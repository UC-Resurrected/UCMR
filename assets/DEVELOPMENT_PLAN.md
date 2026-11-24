# Development Plan

Restore *Uncharted 2: Among Thieves* and *Uncharted 3: Drake’s Deception* multiplayer, with crossplay as the goal for PC, PS3, PS4, and PS5.

[README](../README.md) · [Contributing](../CONTRIBUTING.md)

## 1. Launcher

- [x] Evolve the existing launcher to support this project’s multiplayer features and give it its own identity.
- [x] Support importing assets from players’ own PS3 copies so the launcher can convert the required multiplayer assets locally.
- [x] Let players update the launcher, the multiplayer components, or both together, as well as the choice for manual or automatic updates.
- [x] Move multiplayer networking into the launcher, replacing the current RPCN integration inside Uncharted 3.
- [ ] Evaluate Epic Online Services peer-to-peer networking as an alternative to RPCN, and possiblity implement it, if it's deemed a better solution.

## 2. Uncharted 3 - Windows

- [ ] Re-implement all online dead functions in the PS4 version in a clean way (no directly rewriting PS3 functions, as this is ND code and violates Copyright Infringement).
- [x] Create an asset converter that will recreate PS3 assets in PS4 version - Only assets that do not exist in the PS4 version already (Example: Baseball-shirt Drake is a multiplayer skin but also exists in PS4 single player and with better textures, so we can use that version instead of porting, it makes import faster and reduces work).
- [ ] Fixing all broken gameplay mechanics, aim for 1:1 functionality that PS3 gives.
- [ ] Fix all visual issues, such as missing assets inside a map, or assets in a wrong color, or badly formatted assets.

## 3. Uncharted 2 - Windows

- [ ] Re-implement all online dead functions in the PS4 version in a clean way (no directly rewriting PS3 functions, as this is ND code and violates Copyright Infringement).
- [x] Create an asset converter that will recreate PS3 assets in PS4 version - Only assets that do not exist in the PS4 version already (Example: Baseball-shirt Drake is a multiplayer skin but also exists in PS4 single player and with better textures, so we can use that version instead of porting, it makes import faster and reduces work).
- [ ] Fixing all broken gameplay mechanics, aim for 1:1 functionality that PS3 gives.
- [ ] Fix all visual issues, such as missing assets inside a map, or assets in a wrong color, or badly formatted assets.

## 4. Support Linux and macOS

- [ ] Port all the progress made in Windows to Linux.
- [ ] Port all the progress made in Windows to MacOS.

## 5. Bring PS3 into crossplay

- [ ] Create a PS3 PKG creator inside our launcher that replaces the original multiplayer networking with the RPCN implementation (users will need to own their own copy of the PS3 version) for Uncharted 3.
- [ ] Add a way for PS3 players to sign in, and manage their friends from within the game.
- [ ] Verify that PS3 players can join matches with PC players.
- [ ] Repeat the steps above for Uncharted 2.

## 6. Package the multiplayer ports for PS4 and PS5

- [ ] Build a converter that adapts the PC multiplayer work into PKG packages for PS4 and PS5.
- [ ] Verify that the resulting builds can join cross-platform matches.
