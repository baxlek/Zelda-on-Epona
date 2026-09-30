# Zelda on Epona

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod for The Legend of Zelda: Twilight Princess that keeps
Zelda riding along on Epona at all times while the mod is enabled, just like she does during the game's scripted
final battle horseback sequences.

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
> The grass-whistle horse call is a separate story, though: the game's own horse-call logic special-cases *any*
> moment where Zelda would be riding alone (which, under this mod, is most of the time once Link dismounts) by
> triggering the scripted duel's arrival cutscene instead of Epona's normal gallop-to-player animation — which is
> why you can hear Epona neigh but never see her actually arrive. That path isn't controlled by the flag above, so
> it isn't fixed by this change; working around it would need hooking the game's horse-call function itself, which
> hasn't been attempted yet given the risk already seen from other engine-timing issues (see below).
>
> Separately, enabling the mod while Epona is already loaded in the current scene was reported to cause a crash
> (SIGABRT), and a related issue caused a black screen at boot. Both were traced to the same root cause: enabling
> (or reloading) a mod applies "between frames," so the very first `mod_update` tick after enabling can run outside
> the engine's normal per-frame actor loop, which isn't a safe point to spawn a new actor from. The mod now waits
> ~30 frames after being enabled before attempting its first spawn, so that spawn happens from an ordinary
> per-frame context instead — this has been confirmed fixed. One remaining rough edge: since that first spawn now
> happens a little into normal gameplay rather than during a loading screen, Zelda can visibly "pop in" onto an
> already-visible Epona instead of appearing pre-mounted after a scene change; spawning her exactly in sync with a
> scene transition instead is a possible future improvement.

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
