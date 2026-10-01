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
> vanilla never needed it to be there (only the real duel ever has a horseback-Zelda riding along). An earlier fix
> attempt substituted Epona's root joint matrix wholesale instead, which made things worse (a constant ~90 degree
> orientation error in every situation, not just cutscenes) because that joint's bind pose uses a different basis
> than the logical heading convention. The mod now uses the same technique the game itself already uses for Link's
> own rotation while riding (`daAlink_c::setSyncHorsePos()`): deriving the angle from Epona's *saddle* joint's actual
> rendered matrix, with the same fixed correction that code applies for that joint's bind pose. This makes Zelda
> track Epona's true rendered orientation — including rearing up — in both cutscenes and ordinary gameplay, and only
> affects the mod's own spawned Zelda; the duel's story-placed one is untouched.
>
> That orientation fix initially introduced a new problem: Zelda would sink into Epona's model while rearing up, but
> only in her dual-ride "rear" seat (sitting behind Link), never in her single-rider "actual saddle" seat, nor in
> other cutscenes where root and saddle diverge a lot, such as Epona being led by Link or ridden by a bulblin. That's
> because that attempt also re-derived Zelda's *position* from the saddle joint — but her seat offsets were never
> calibrated against the saddle joint; they're multiplied through the *root* joint in the original, unmodified
> function, which is exactly why the root-based position was already correct in every one of those other cases.
> Moving the anchor point to the saddle joint's own translation put it in the wrong place; it only became noticeable
> in an extreme pose like rearing. The mod now keeps the anchor point exactly as the original function computes it
> (from the root joint), and only re-expresses the *direction* of the local seat offset using the corrected rotation
> above, keeping Zelda's seat glued to Epona's actual surface without disturbing the anchor point itself.
>
> One of the game's earliest story cutscenes shows Epona carrying bundles of firewood — a separate decorative prop
> placed at the saddle position for that one shot, unrelated to Epona's own model and always present regardless of
> any horseback-Zelda. With this mod keeping Zelda riding at all times, she now ends up sharing the saddle with it.
> The mod removes that prop whenever it's actually coincident with Epona, as long as this mod has a Zelda spawned at
> all (rather than only once she's confirmed attached as the rider, since actor creation/attachment can straddle
> frames and that check wasn't reliably true yet on the exact frame the prop loads in), leaving any other, unrelated
> decorative bundles placed elsewhere in the game untouched.

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
