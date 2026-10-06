# Zelda on Epona

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod that keeps Zelda riding along on Epona at all times 
while the mod is enabled, just like she does during the game's scripted final battle horseback sequences.

Whenever Epona is present and no horseback-Zelda passenger already exists (e.g. from a story cutscene), the mod
spawns one so she stays on the saddle. She remains there even after Link dismounts, sitting alone up front just as
the game's own horseback-Zelda actor already supports; any story-placed horseback Zelda (such as the horseback
archery duel, where she rides alone) is never touched or duplicated.

> [!NOTE]
> The game normally treats *any* horseback-Zelda passenger as a sign that the scripted duel against Ganondorf is
> underway (Link's `FLG2_HORSE_ZELDA` player flag), which blocks normal dismounting and makes Wolf Link/Midna
> unreachable. Since the actual duel always places its own horseback-Zelda actor (never the one this mod spawns),
> the mod now clears that flag back off every tick while *its own* Zelda is riding, leaving it untouched whenever a
> story-placed one is present (i.e. during the real duel, where dismounting and Midna should stay blocked as usual).
>
> The grass-whistle horse call is fixed the same way: the game's own horse-call logic special-cases *any* moment
> where Zelda would be riding alone (which, under this mod, is most of the time once Link dismounts) by triggering
> the scripted duel's arrival cutscene instead of Epona's normal gallop-to-player animation — which is why you'd
> hear Epona neigh but never see her actually arrive. The mod now hooks that horse-call function directly and briefly
> detaches its own Zelda from the horse right before the call runs (she's reattached again by the very next tick, the
> same way the flag above is), so the game takes its normal path instead. As with the flag fix, a story-placed
> horseback Zelda is never touched, so the real duel's behavior is unaffected.
>
> Separately, enabling the mod while Epona is already loaded in the current scene was previously reported to cause a
> crash (SIGABRT), and a related issue caused a black screen at boot. Both were originally worked around with a ~30
> frame warmup delay before the mod's first actor spawn, based on a "between frames" theory (enabling/reloading a mod
> can land the very first `mod_update` tick outside the engine's normal per-frame actor loop). Once the "double
> Zelda" root cause below was fixed, this same SIGABRT (enabling the mod while already riding Epona) stopped
> reproducing even with the warmup delay removed entirely, confirming the delay had only ever been masking that same
> underlying actor-creation-in-progress issue rather than a distinct "between frames" problem. The delay has since
> been removed, so Zelda now spawns on the very first `mod_update` tick after enabling instead of a little into
> normal gameplay.
>
> There have also been reports of two overlapping Zelda models appearing at once while riding. Debug logging confirmed
> the mod itself was spawning two separate `HoZelda` actors back-to-back, a couple of frames apart, every single time
> its spawn condition fired. The root cause: actor creation (`fopAcM_create`) isn't instantaneous — the new actor sits
> in the engine's internal "create queue" for a frame or more (e.g. while its resources load) before it's fully
> created, and while that's happening, the engine's own actor lookup (`fopAcM_SearchByID`) reports it as "not found,"
> even though it isn't actually gone yet. The mod used to treat that as "our spawned Zelda disappeared," giving up on
> it and immediately spawning a second one while the first was still mid-creation. The mod now checks whether its
> tracked actor is still being created (`fpcM_IsCreating`) before concluding it's gone, so it no longer spawns a
> duplicate while the first is still in flight — this has been confirmed fixed via log analysis, but please report
> back if it's still reproducible in-game.
>
> Zelda used to face the wrong way (often straight towards in-game North) and ignore Epona's own movement — e.g. not
> leaning back when Epona reared up — during some story cutscenes. `daHoZelda_c::setMatrix()` orients her by copying
> Epona's separate, logical heading field (`shape_angle`) wholesale; that field is normally kept in lockstep with
> Epona's own rendered pose, but scripted cutscenes that move Epona via demo data don't always keep it in sync, since
> vanilla never needed it to be there (only the real duel ever has a horseback-Zelda riding along) — leaving
> `shape_angle` stale while Epona's actual animated pose (including rearing up) keeps changing underneath it.
>
> Several earlier attempts at fixing this by *decomposing* a joint matrix into Euler angles and/or recomputing
> position from it (with some fixed correction for that joint's own bind-pose twist, the same technique
> `daAlink_c::setSyncHorsePos()` uses for Link's own rotation while riding) all regressed in one way or another — a
> constant ~90 degree orientation error, sinking into Epona's model while rearing, floating next to her instead, or
> ending up noticeably off her normal seat even outside of rearing. The key realization was that every one of those
> regressions showed up specifically in the dual-ride seat (Zelda riding behind Link) — because that seat already
> works correctly via vanilla's own `shape_angle` logic in the large majority of cutscenes, which is the one case
> this engine code was actually written and tested for. It's only the *solo* seat (no Link riding, which only this
> mod's standalone Zelda ever uses) where `shape_angle` goes stale and needs a fix at all.
>
> The mod now scopes its override to the solo seat only, leaving the dual-ride seat's vanilla computation completely
> untouched. For the solo seat, Zelda's position is left exactly as the original, unmodified function already
> computed it, and only the rotation used to draw her is replaced — derived from `horse->getSaddleMtx()` (a live,
> animated joint matrix that reacts to rearing, unlike the separate `shape_angle` field) via the same
> `mDoMtx_MtxToRot()` + bind-pose correction technique `daAlink_c::setSyncHorsePos()` uses for Link. Since the
> dual-ride seat is never touched, it can never regress, and the solo seat's position is never recomputed, so it
> can never sink, float, or shift off-center. This also matches what was independently observed in-game: Zelda's
> position had only ever been reported wrong in the dual-ride (actual saddle) seat, never while riding solo, being
> led by Link, or being ridden alongside a bulblin.
>
> One known remaining edge case: a few dual-ride cutscenes (e.g. right after the first King Bulblin duel on Eldin
> Bridge) still don't react to Epona's movement correctly, since that seat's vanilla logic isn't touched by this
> fix. This is a pre-existing, narrower issue left for future investigation rather than something this fix attempts
> to solve.
>
> Separately from her orientation, Zelda's *animation* also didn't react to Epona rearing up, and a few individual
> story cutscenes had their own quirks — e.g. one of the earliest shows Epona carrying bundles of firewood that end
> up sharing the saddle with Zelda once she's always present. Patching each of these up individually (as was tried)
> kept surfacing new, similarly-themed issues with no end in sight, since every story cutscene that drives Epona
> directly risks a new mismatch with a permanent extra rider that vanilla was never designed around.
>
> Rather than continuing down that path, the mod now hides its own spawned Zelda for the duration of any *scripted*
> story cutscene by default (detected via `daHorse_c::checkHorseDemoMode()` — true any time something other than the
> player is driving Epona, which covers every cutscene proc she can be puppeted through, not just the single
> "tool demo" state an earlier version of this check looked at), and she reappears automatically once the cutscene
> ends. This is purely a visibility toggle: cutscenes that already feature their own story-placed Zelda (such as
> the horseback archery duel against Ganondorf) are completely unaffected either way, since the mod only ever
> manages the actor it spawned itself.
>
> The only exception is the opening title screen (Link/Epona galloping across Hyrule Field), which drives Epona
> through the exact same `checkHorseDemoMode()` plumbing as a real cutscene but should always show her regardless
> of the toggle (`is_title_screen()`). Every other cutscene-like event — including the horse-call/grass-whistle
> gallop-back, NPC conversations, and area/scene transitions — is treated the same as any other story cutscene and
> hidden by default; she's never kept visible for those just because the horse happens to only briefly be in demo
> mode for them.
>
> Hiding her is no longer done by deleting and later recreating the actor. That approach — and a more direct
> "just unload/reload her resources" one before it — both caused a SIGABRT, almost certainly because recreating an
> actor (`fopAcM_create`) isn't safe immediately around a cutscene boundary. Instead, she's now hidden in place: a
> hook on `daHoZelda_c::setMatrix()` overrides the matrix used to draw her for the frame, shrinking her model down
> to an imperceptible size while leaving the actor itself alive and ticking normally the entire time. She's back to
> normal the instant hiding ends, with no recreation (and no extra delay) needed.
>
> An earlier version of this hiding also moved her model far underground on top of shrinking it, but that dragged
> Epona's reins down with her — they're attached to Zelda's hand while she's riding solo, so moving her position
> visibly stretched them downward every time she was hidden. Shrinking her scale alone, without touching her
> position, avoids that: her seated position (and the reins' attachment point) never moves, she's just rendered too
> small to make out.
>
> If you'd rather see Zelda during story cutscenes too (with the orientation fix above still applying to her solo
> seat), a **Show Zelda during cutscenes** toggle is available in this mod's panel in the in-game Mods window,
> off by default. Flipping it mid-cutscene only takes effect starting with the *next* cutscene: spawning an actor
> while a cutscene is actively puppeting the horse from scripted demo data isn't safe (it crashed rather than just
> appearing a frame late), so turning the toggle on while a cutscene is already playing doesn't spawn her into that
> cutscene if she wasn't already present, it only applies going forward.

> [!NOTE]
> AI can get WORDY! 

See the [Dusklight modding documentation](https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md)
for the full mod API: services, hooking game functions, asset overlays, and more.

## Quick start

1. Edit `mod.json.in`: set your mod's `id` (reverse-DNS style, e.g. `com.example.my_mod`),
   `name`, `author`, and `description`.
2. Rename the target in `CMakeLists.txt` (`add_mod(zelda_on_epona ...)`) (this names the `.dusk` file).
3. Write your mod in `src/mod.cpp`.
4. Build locally:
   ```sh
   cmake -B build
   cmake --build build
   ```

The result is `build/mods/<name>.dusk`. Copy it into the game's mods folder to try it:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

During development, rebuild, copy and click **Reload** in the in-game mod manager to pick up changes.

> [!IMPORTANT]
> A mod built locally will only be valid for your own platform, and shouldn't be distributed.
> The repository will build a [cross-platform bundle](#github-actions) for distribution. See below.

## Updating to a new Dusklight version

Change the `DUSKLIGHT_VERSION` line in `CMakeLists.txt` to the new release tag (or commit hash) and reconfigure. The
pinned version is fetched into `dusklight/` automatically. Use the `dusklight/` checkout to browse game code, headers
and mod services.

> [!IMPORTANT]
> The Dusklight checkout is for **reference only**. Mods use
> [services](https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md#built-in-services) and
> [hooks](https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md#hooking-game-functions) to interact with
> game code.

## GitHub Actions

The included GitHub Actions workflow builds the mod for the following platforms:
- Windows (AMD64 & ARM64)
- macOS (Apple Silicon & Intel)
- iOS (Apple Silicon)
- Linux (x86_64 & aarch64)
- Android (aarch64)

It then merges the per-platform builds into a single `.dusk` supporting all platforms. (Artifact `mod-combined`) 

Pushing a tag to the repository creates a GitHub release with the combined bundle.

## For Dusklight developers

Point the build at an existing checkout instead of fetching one:

```sh
cmake -B build -DDUSKLIGHT_DIR=~/path/to/dusklight
```
