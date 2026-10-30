# Free Touch

Free Touch is a BakkesMod training plugin for Rocket League's offline freeplay. It has:
- drills with scored reps, sessions and a daily challenge;
- training pack tools (a bot defender, net targets);
- recorded shots;
- bots to play against.

Its training only runs in freeplay (`GameWrapper::IsInFreeplay()`), never in online matches.

This is the source of the version on BakkesPlugins, which installs it and keeps it up to date. The notes below cover
how it builds, its data files and what it sends online, for BakkesPlugins' moderators and anyone else reading the code.
Credits for the bots, fonts and libraries are in `THIRD_PARTY.md`.

## Building

- Release | x64, from `FreeTouch.sln`, made from Martinii89's BakkesmodPluginTemplate. The SDK comes from
  `BakkesMod.props` (the template's registry lookup); the only post-build step is the template's `bakkesmod-patch`.
- Third-party code: nlohmann-json from vcpkg (`vcpkg.json`, manifest mode, static triplet). Dear ImGui is the
  template's copy in `src/IMGUI/`. Nothing else is downloaded during the build.
- C++20 (`/std:c++20`), toolset v145 (Visual Studio 2026). It should build with v143 as well if you change
  `PlatformToolset`.

## Data files

`data/` holds the plugin's data files, laid out as they belong under `%APPDATA%\bakkesmod\bakkesmod\data\`. The plugin
reads them from `GameWrapper::GetDataFolder() / "freetouch" / "content"`:

- `scenarios/`, `sessions/`, `routines/`, `daily/`, `tutorials/`, `schema/`: JSON (drills and their schema).
- `bots/necto/policy.bin`, `bots/nexto/policy.bin`: neural network weights (a short header, then float32 tensors), read by
  `src/bots/policy/EarlPolicy.cpp`. They are data, not code. See `THIRD_PARTY.md` for where they come from.
- `fonts/` (Barlow, SIL OFL), `images/` (menu art made for Free Touch), `sounds/` (WAV, synthesised for Free Touch).

Free Touch writes files only under `data/freetouch/` (settings, logs, recorded shots). To offer shots friends sent,
it reads the player's Downloads folder, looking only at small shot files with Free Touch's own extension. A shot is
copied to the clipboard as a file when the player shares it. Shot files are JSON, and the plugin checks them before
importing.

## Network

All requests go through one client, `BakkesGameAdapter::http` (`src/game/`, `HttpWrapper::SendCurlRequest`), to Free
Touch's own Supabase project (`src/core/Online.h`: its URL and public "publishable" key, meant to be public; access is
enforced by row-level security on the service). It is optional. Without an account, everything except the online
features works offline. No requests are sent while the player is in an online match.

Players who pick a name in Settings > Account send:
- their name and friend code (`profiles`);
- their training minutes this week, shown to friends (`activity`);
- shots they choose to share (`shots`, `shot_deliveries`);
- Daily Challenge scores (`daily_scores`);
- race times with ghost paths (`post_race_time`).

The plugin reads leaderboards and friends' shares back. Sign-in is Supabase's anonymous auth (`/auth/v1/signup`,
`/auth/v1/token`). The plugin downloads no files and runs no downloaded code: replies are JSON, parsed with
nlohmann-json.

This build has no self-updater: BakkesMod installs new versions from BakkesPlugins. Copies shared outside BakkesPlugins
have one; this build leaves it out with `#define FT_SELF_UPDATE 0` in `src/flavor.h`, and the updater's source files
aren't in this zip.
