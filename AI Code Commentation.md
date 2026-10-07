# AI Code Commentation

This file preserves every explanatory code comment that used to live inline in `src/mod.cpp`, kept here instead so the source file stays focused on code while this context remains accessible. Entries are listed in the same order they appear in `src/mod.cpp`, each labeled with its original line range and a short snippet of the code it was documenting at the time this file was generated (line numbers will drift from the current file as it's edited further).

## Line 12

Precedes: `#include "d/actor/d_a_arrow.h"`

Game includes

## Lines 46-51

Precedes: `static ConfigVarHandle g_cvarShowInCutscenes = 0;`

Whether this mod's own spawned Zelda should stay visible on Epona during scripted story cutscenes. Off by default: by default she's hidden for the duration of any such cutscene (and reappears once it ends) rather than potentially clashing with the scene's own staging (wrong orientation/animation, overlapping props, etc.) the way earlier attempts at patching her up for every individual cutscene kept doing. Exposed as a single toggle in the mod's panel in the host Mods window.

## Lines 63-66

Precedes: `static ConfigVarHandle g_cvarAutoTargetEnemies = 0;`

Whether this mod's own spawned Zelda should auto-target and fire her Light Arrows at nearby enemies, the same way she already does at Ganondorf during the real horseback duel, instead of just riding passively. Off by default ("Zelda active in combat" toggle). Exposed as a toggle in the mod's panel in the host Mods window for anyone who'd rather she actively fight.

## Lines 78-84

Precedes: `static constexpr bool kDiagnosticLoggingEnabled = false;`

Diagnostic logging added while investigating the "Zelda stays hidden after certain cutscenes"/SIGABRT bugs is disabled now that those issues are resolved, but every diagnostic code path below is deliberately kept in place (not removed) in case it's needed again for a future investigation. Flip this to `true` to re-enable all of it at once; there is deliberately no runtime toggle for it anymore (the previous "Log cutscene diagnostics" UI toggle/config var has been removed), since re-enabling it is expected to be a rare, developer-only action rather than something a player would need to reach from the Mods window.

## Lines 87-88

Precedes: `static const char* kHoZeldaStageName = "HoZelda";`

The stage name used by the game to load the horseback-Zelda actor (see `d_stage.cpp`'s OBJNAME table: OBJNAME("HoZelda", fpcNm_HOZELDA_e, -1)).

## Lines 91-93

Precedes: `static ActorId s_spawnedZeldaId = 0;`

The ActorId of the horseback-Zelda actor this mod spawned, if any. We only ever manage an actor we spawned ourselves; a HoZelda actor placed by the vanilla game (e.g. during the horseback archery duel against Ganondorf, where Zelda rides alone) is always left untouched.

## Lines 97-100

Precedes: `static s32 s_lastKnownZeldaRoomNo = -1;`

The room number our spawned Zelda was last confirmed alive in, and whether the most recent engine-initiated loss of her (if any) happened without the room changing. Both are updated by `mod_update()` and consumed by the `inScriptedCutscene` branch's recovery respawn below -- see that branch's own comment for why the recovery respawn must never fire on a cross-room loss.

## Lines 109-116

Precedes: `if (fpcM_IsCreating((fpc_ProcID)s_spawnedZeldaId)) {`

Actor creation (`fopAcM_create`/`fpcSCtRq_Request`) is a multi-phase request that can take more than one frame to complete (e.g. resource loading), during which the new actor sits in the engine's "create queue" rather than its normal execute queue. While that's happening, `fopAcM_SearchByID` reports "not found" (a null actor, but still a success return) even though the actor we spawned is still on its way and hasn't actually disappeared. Treating that as "the actor is gone" made us give up tracking it and spawn a second HoZelda while the first spawn request was still in flight, producing two overlapping HoZelda actors (the "double Zelda" bug). `fpcM_IsCreating` lets us tell the two cases apart.

## Lines 124-126

Precedes: `s_hasSpawnedZelda = false;`

The actor we spawned no longer exists (e.g. deleted on a room change), or its ProcID has since been recycled by the engine for an unrelated actor (ProcIDs are reused once freed). Either way, we no longer have a spawned Zelda to track.

## Lines 133-140

Precedes: `static bool is_spawned_zelda(const fopAc_ac_c* actor) {`

Whether `actor` is the HoZelda actor this mod spawned itself (as opposed to a story-placed one, e.g. the real Ganondorf duel's own Zelda), used throughout to scope hooks to our own actor only.

NOTE: this dereferences `actor` (via `fopAcM_GetID`), so it is only safe to call with an actor pointer that is already known to currently be alive (e.g. one just handed to us as a hook's own argument, or one just returned by `fopAcM_SearchByID`/`fopAcIt_Judge`). It must NOT be called with `daHorse_c::getZeldaActor()`'s return value directly -- see `is_spawned_zelda_attached()` below for why, and use that instead for that specific case.

## Lines 146-165

Precedes: `static bool is_spawned_zelda_attached(daHorse_c* horse) {`

Whether the HoZelda actor currently attached to `horse` (`daHorse_c::getZeldaActor()`, i.e. `m_zeldaActorKeep.getActor()`) is this mod's own spawned Zelda -- the safe way to ask that question, unlike `is_spawned_zelda(horse->getZeldaActor())`.

`m_zeldaActorKeep` is a cached raw pointer, refreshed (`setActor()`, which re-resolves it via `fopAcM_SearchByID` under the hood) only once per tick, at the very start of `daHorse_c:: execute()`. A captured dusklight crash log showed a SIGABRT from exactly this unsafe pattern used in the grass-whistle hook below: the whistle's own call into `daHorse_c::callHorse()` is driven by Link's actor (`daAlink_c`'s whistle item procedure, `d_a_alink_whistle.inc`), not by `daHorse_c::execute()` itself, so there is no guarantee `execute()` has already run -- and therefore refreshed `m_zeldaActorKeep` -- earlier in the same tick. If the attached Zelda actor was deleted by the engine since the *previous* tick (e.g. as part of a room/layer reload, like the one already documented on `find_spawned_zelda()` above) but `execute()` hasn't run yet this tick to notice and clear the cache, `getZeldaActor()` still returns that now-dangling pointer, and dereferencing it (as `is_spawned_zelda()`'s `fopAcM_GetID()` call does) reads freed memory.

`fopAcM_SearchByID()` never dereferences the pointer it's given back by value here -- it looks the actor up fresh by ID in the engine's live actor table -- so comparing its result against the (potentially dangling) attached pointer by address alone, rather than by dereferencing the attached pointer itself, avoids that crash entirely.

## Lines 176-179

Precedes: `static bool s_hideSpawnedZeldaInCutscene = false;`

Whether the mod's own spawned Zelda should currently be visually hidden (shrunk to an imperceptible size) rather than deleted, set once per tick by `mod_update()` and read back by the `HoZeldaSetMatrix` hook below, which does the actual hiding. See that hook's comment for why hiding her in place -- instead of deleting and later recreating the actor -- is necessary.

## Lines 182-188

Precedes: `static bool is_title_screen() {`

The opening title screen (Link/Epona galloping across Hyrule Field) is driven through the same `dScnPly_c` gameplay scene class as ordinary play, just requested under a different proc name (`fpcNm_OPENING_SCENE_e` instead of `fpcNm_PLAY_SCENE_e`, see `dComIfG_changeOpeningScene()` in `d_com_inf_game.cpp`), and that scene drives Epona exactly the same way a scripted cutscene does. Without specifically excluding it, that would make the title screen itself count as a "scripted cutscene" and hide this mod's Zelda there by default -- but she should always be visible on the title screen regardless of the cutscene-visibility toggle.

## Lines 194-246

Precedes: `static bool names_equal_case_insensitive(const char* a, const char* b) {`

A handful of story cutscenes (currently "Demo01_01", "Demo01_02", "Demo36_01"/"Demo36_02" and "demo90") have their own staging/camera work built around Epona *not* visibly carrying a second rider, so this mod's own spawned Zelda must stay hidden through them no matter what the "Show Zelda during cutscenes" toggle is set to. Every other cutscene still respects that toggle normally.

One earlier version of this check matched against `dStage_roomControl_c::getDemoArcName()`, the name of the currently-loaded cutscene demo *resource archive* -- but `loadDemoArchive()` only ever loads a new archive into an empty name slot as part of a *room* being created, and deliberately leaves it alone afterwards so any number of distinct cutscenes sharing that room can reuse its one cached archive without reloading it, so it never updates again once a later, different cutscene starts sharing that same room.

A second version instead matched `dComIfGp_getEvent()->mEventId` (via `dEvDtEvent_c::getName()` off of `dEvent_manager_c::getEventData()`) -- `dEvt_control_c::mEventId` only gets assigned by that class's own `setParam()`, called from its `demoCheck()`/`talkCheck()`/etc. "order accepted" paths, so any cutscene instead ordered directly against `dEvent_manager_c::order()` (e.g. via a map-tool-triggered event, bypassing `dEvt_control_c` entirely) leaves `mEventId` stuck on whatever the last `dEvt_control_c`-routed event happened to be -- which is exactly backwards from a real signal and why that version hid the wrong cutscenes and showed the right one.

`dEvent_manager_c` tracks the event it is *itself* actually running completely independently of `dEvt_control_c`, in its own `mCurrentEvId` member -- set by its own `order()` regardless of which caller invoked it -- and exposes it pre-built via `getRunEventName()` (named exactly for this purpose: it already returns "NO DATA"/"NOT RUNNING" sentinel strings, neither of which collide with any real cutscene name below, for the "nothing is running" and "between cuts" cases respectively instead of requiring extra null-checks here).

A captured diagnostics log (see `log_cutscene_diagnostics_if_changed()`) from an actual playthrough of the Demo01 sequence showed `getRunEventName()` returning all-lowercase event names at runtime -- "demo01_01", "demo01_02", "demo01_03", and (for the title screen's own cutscene-like event) "demo38_01" -- while this list was written with the capitalized demo *archive*/file naming convention ("Demo01_01", "Demo01_02"). Because the comparison below used to be case-sensitive, none of these names ever actually matched anything, silently making this entire forced-hide list dead code; the comparison is now case-insensitive so it matches regardless of which convention a given event name happens to use.

"Demo01_03" was briefly added here on the (incorrect) assumption that it was staged the same way as "Demo01_01"/"Demo01_02" just because it's part of the same numbered sequence sharing the same room -- but per direct confirmation, only "Demo01_01" and "Demo01_02" are actually staged without a second rider; "Demo01_03" should respect the "Show Zelda during cutscenes" toggle like any ordinary cutscene, so it does not belong in this forced-hide list.

"Demo36_00" had the exact same kind of naming mismatch as the case-sensitivity bug above, just one level deeper: a captured diagnostics log from an actual Demo36 playthrough showed `getRunEventName()` only ever returning "demo36_01" and "demo36_02" for Demo36's own two actual cutscene segments -- "demo36_00" never appears as a *running event* name at all (only as the `demoArc` archive name, which this list is not compared against) -- so this entry was dead code exactly like the Demo01 names used to be, and Zelda stayed visible for the user's entire Demo36 playthrough despite the toggle being off. Replaced with the two real sub-event names confirmed by that log. "Demo90_00" had the exact same archive-name-vs-running-event-name mismatch: confirmed its real running event name is "demo90" (singular, no "_00" sub-cut suffix), so it was replaced with that name below.

## Lines 275-277

Precedes: `struct CutsceneDiagnosticsSnapshot {`

Snapshot of every signal `mod_update()` consults to decide whether this mod's own spawned Zelda should be hidden this tick. Kept as a plain struct (rather than just a block of local variables) so it can be compared whole against the previous tick's snapshot below.

## Lines 310-314

Precedes: `static void log_cutscene_diagnostics_if_changed(daHorse_c* horse, bool titleScreen,`

Logs every signal feeding into the cutscene hide/show decision above, but only the instant any of them actually changes from the previous tick -- logging unconditionally every tick would flood the log with hundreds of identical lines per second during ordinary gameplay. Gated behind `kDiagnosticLoggingEnabled` (currently `false`) so it never runs unless that's flipped back on for a future investigation.

## Lines 362-363

Precedes: `static void* judge_other_hozelda(fopAc_ac_c* i_actor, void*) {`

`fopAcIt_Judge()`'s filter for `find_other_hozelda()` below: matches any `HoZelda` actor other than our own spawned one.

## Lines 371-385

Precedes: `static fopAc_ac_c* find_other_hozelda() {`

Finds a `HoZelda` actor somewhere in the world that this mod did *not* spawn itself -- i.e. one placed by the story, such as the real horseback archery duel against Ganondorf (the only place in the vanilla game a `HoZelda` actor is ever placed directly rather than through this mod).

Used to detect that real duel and bow out of its way entirely: this mod otherwise has no way to tell "the story is about to attach its own HoZelda to the horse" apart from the moment it actually happens, which can be one or more frames after this mod's own spawn/keep-mounted logic already ran for that same frame -- `d_a_horse.cpp`'s `execute()` only overwrites whichever actor is currently attached once the story's own HoZelda starts ticking, so there's otherwise a window where both our own spawned Zelda and the story's are simultaneously alive (the "duplicate Zelda" bug), and -- since our own hooks are scoped to our own actor via `is_spawned_zelda()` above -- our own Zelda would keep searching for and firing at nearby enemies the whole time, overlapping the real duel's own Ganondorf-only targeting. Checking for *any* other HoZelda in existence, rather than just whichever is currently attached to the horse, catches that window regardless of exactly which frame the handoff happens on.

## Lines 419-439

Precedes: `DEFINE_HOOK(static_cast<s32 (*)(fopAc_ac_c*)>(&fopAcM_delete), ActorDelete);`

--- Diagnostics: detect engine-initiated deletion of our own spawned Zelda -------------------

A captured diagnostics log (see `log_cutscene_diagnostics_if_changed()`) showed `hasSpawnedZelda` flipping from true to false on the very tick the running event name changed from one sub-cutscene to the next (e.g. "demo01_01" -> "demo01_02", both still the same room and layer), while none of this mod's own four `remove_spawned_zelda()` call sites in `mod_update()` were active that tick (`storyHoZeldaActive` stayed false throughout that log, the horse was never null, and `horseDemoMode` never dropped). A follow-up raw dusklight engine log confirmed the actual mechanism: switching to the next sub-cut recreates the entire room's actor list from scratch (visible as a fresh `fpcBs_Create` for both `fpcNm_HORSE_e` and our own `fpcNm_HOZELDA_e` right at the transition) -- and this hook never fires for her during that reload, meaning the bulk teardown that precedes it doesn't go through this particular `fopAcM_delete` overload at all (our own later `remove_spawned_zelda()` cleanup call for the already-gone old actor is what the dusklight engine's own actor service logs as "doesn't exist", not this hook). `mod_update()`'s `inScriptedCutscene` branch now has a recovery respawn for this exact case.

This hook is kept as a no-behavior-change diagnostic safety net: it logs the exact moment (and surrounding engine state) our own tracked actor is deleted through this specific generic actor-deletion entry point by anything other than this mod's own `remove_spawned_zelda()`, in case some other, not-yet-observed code path does go through it.

## Lines 462-483

Precedes: `DEFINE_HOOK(&daHorse_c::callHorseSubstance, HorseCallSubstance);`

`daHorse_c::callHorseSubstance()` (the grass-whistle horse call) special-cases *any* moment where the current HoZelda passenger is riding alone (`checkSingleRide()`, true whenever Link isn't mounted) by assuming the scripted final-duel reunion is underway: instead of the normal gallop-to-player behavior, it silently plays Epona's neigh, jumps straight to the duel's "arrival" demo state, and never actually moves Epona. Since this mod keeps Zelda riding solo for the entire game (not just the real duel), that special case would otherwise fire every time the whistle is used while she's mounted.

A real scripted duel always uses its own, story-placed HoZelda actor, never one this mod spawned. So immediately before the original runs, if the actor currently attached to the horse is ours, we briefly detach it (`setZeldaActor(nullptr)`) so `callHorseSubstance` takes its normal path instead. This has no lasting effect: `daHoZelda_c::execute()` unconditionally reattaches our Zelda to the horse (`horse->setZeldaActor(this)`) on every single tick anyway, so by the very next frame she's back exactly where she was.

This hook fires from `daHorse_c::callHorse()`, which -- unlike most of the other HoZelda-related hooks in this file -- is not called from `daHorse_c::execute()` at all, but from Link's own grass-whistle item procedure (`d_a_alink_whistle.inc`). `horse->getZeldaActor()` must therefore be checked via `is_spawned_zelda_attached()`, not `is_spawned_zelda()`, to avoid dereferencing a cached actor pointer that may not have been refreshed yet this tick -- see that helper's own comment, which documents a captured crash log of exactly this call site SIGABRT-ing on a dangling pointer left over from a room reload between ticks.

## Lines 494-537

Precedes: `DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);`

`daHoZelda_c::setMatrix()`'s own, unmodified computation for the solo (front saddle) seat -- `current.pos` from a fixed local offset through `horse->getRootMtx()`, `shape_angle` copied straight from `horse->shape_angle` -- is value-for-value the same technique `daAlink_c::setSyncHorsePos()` uses for Link's own riding position outside of a few specific movement animations (`mDoMtx_multVec(horse_p->getRootMtx(), &l_localHorseRidePos, &current.pos); shape_angle = horse_p->shape_angle;`), just with a different local offset constant. Since Link, using that exact computation, never clips into or floats off of Epona while she's startled and rears up, the computation itself isn't the problem; every attempt at improving on it instead (deriving rotation from the live saddle joint; re-projecting Zelda's local offset through that joint; nudging only its vertical component) ended up regressing one axis while fixing another (leaning too far back; clipping into Epona's risen body; floating above the saddle), because Zelda's own local offset sits much further forward and higher than Link's -- any deviation from the plain root-matrix computation gets amplified by that larger lever arm in a way it never does for Link.

So rather than attempting to derive a better position/rotation for the rearing case, this just borrows Link's own riding position outright for it: while Epona is mid-rear (`horse->checkTurnStand()`), Zelda's seat is placed using Link's own local ride offset (`daAlink_c`'s `l_localHorseRidePos`, duplicated below since the original is a file-local constant in `d_a_alink.cpp`) through the exact same root-matrix-plus-direct-shape_angle-copy computation already proven not to clip or float for Link in that same state. Outside of a rear, this hook does nothing at all: vanilla's own computation (with Zelda's own local offset) was never reported to look wrong there, so it's left completely untouched.

`checkTurnStand()` only reports true for the held-rear portion in the *middle* of Epona's whole startle/turn animation (see `daHorse_c::procTurn()`'s `field_0x1780`/`field_0x1774` frame-window check), not its full duration. Swapping the local offset outright the instant that window opens (and again the instant it closes) was an abrupt, one-frame jump between Zelda's own offset and Link's -- small, but visible as a brief backward slide right as the rear kicks in. To smooth that over, the two offsets are now blended across a few frames (via `cLib_chaseF`) instead of swapped in one step, so Zelda's seat eases from one to the other alongside the rear starting/ending rather than snapping.

Scoped to our own spawned Zelda only: the vanilla duel's story-placed HoZelda already works correctly as-is, so it's left untouched to avoid any risk of regressing it. The rear-blend fix above is additionally scoped to the solo seat only (no Link riding): the dual-ride (rear) seat already works correctly via vanilla's own computation outside of cutscenes and was never part of that particular clipping/floating bug.

This same post-hook also fixes a separate rotation-mismatch bug during scripted cutscenes, for both the solo seat AND the dual-ride (Link also riding) seat: see the `inCutscene` branch further down in the hook's body for why a plain `shape_angle = horse->shape_angle` copy (vanilla's own, unconditional, already-correct technique for ordinary gameplay) isn't actually enough there, and what's done differently instead.

## Lines 540-559

Precedes: `static const f32 kHiddenScale = 0.0001f;`

How small a scale to shrink this mod's own spawned Zelda's rendered model to while she should be hidden during a scripted cutscene, small enough to be imperceptible to the player regardless of how close the cutscene's camera gets.

This replaces an earlier approach of simply deleting (`fopAcM_delete`) the actor for the cutscene's duration and recreating it once the cutscene ended: `fopAcM_create` isn't safe to call while `daHorse_c` is being puppeted by scripted demo data (see `spawn_zelda_on_horse()`'s callers below), but more importantly, deleting *and recreating* her around every cutscene boundary was found to crash (SIGABRT) outright, almost certainly for the same underlying reason. Hiding her in place -- she's never deleted, just shrunk down where the player can't make her out -- sidesteps that entirely: the actor is always alive and ticking normally, so there's no unsafe create/delete anywhere near a cutscene boundary, and she reappears the instant the cutscene ends with no recreation (and no multi-frame delay) needed at all.

An earlier version of this hiding also moved her model far underground, on top of shrinking it. That caused its own glitch: Epona's reins are attached to Zelda's hand whenever she's loaded and riding solo, so dragging her model's position itself down along with it dragged the reins straight down with her, visibly stretching them to that underground position every time she was hidden. Scale alone doesn't have that problem -- shrinking her model leaves her seated position (and thus the reins' attachment point) exactly where it already was, just imperceptibly small.

## Lines 570-572

Precedes: `static f32 s_turnStandBlend = 0.0f;`

How much of a rear's local-offset blend (0 = fully vanilla, 1 = fully Link's offset) has eased in so far; chased every frame toward 0 or 1 depending on `checkTurnStand()` so the transition is smooth in both directions instead of an instant swap.

## Lines 581-585

Precedes: `if (s_hideSpawnedZeldaInCutscene) {`

Hiding her for a cutscene overrides everything else below: her position/rotation/animation don't matter while she's imperceptible underground, and `current.pos`/`shape_angle` (her actual logical transform, read by other systems such as the horse-rider-flag clearing in `mod_update()`) are deliberately left untouched -- only the matrix actually used to draw her this frame is overridden, so she's back exactly where she belongs the instant hiding ends.

## Lines 594-596

Precedes: `bool dualRide = daPy_getLinkPlayerActorClass()->checkHorseRide();`

Dual-ride (Link also on Epona) is still in scope for the cutscene rotation fix further down (see `inCutscene` below), just not for the rear-blend fix right above: that one addresses a solo-seat-only clipping/floating bug that vanilla's own dual-ride computation never had.

## Lines 605-608

Precedes: `if (dualRide) {`

Eases toward 1 while rearing, back toward 0 once the rear ends; 1/8th per frame fully blends in about 8 frames (roughly a tenth of a second at 60 FPS), fast enough to not be noticeable as its own separate motion, but slow enough to erase the one-frame snap. Only chased while riding solo -- see the comment on `dualRide` above.

## Lines 615-616

Precedes: `bool inCutscene = horse->checkHorseDemoMode();`

Whether a scripted cutscene (as opposed to ordinary player-driven riding) is currently puppeting Epona.

## Lines 619-621

Precedes: `if (!inCutscene && (dualRide || s_turnStandBlend <= 0.0f)) {`

Fully settled outside of a rear and outside of a cutscene: vanilla's own computation (already run by the time this post-hook fires) is left completely untouched. For the dual-ride seat, that's simply "outside of a cutscene" (the rear-blend fix never applies to it at all).

## Lines 626-629

Precedes: `static const Vec kZeldaFrontHorseRidePos = {-75.893997f, 57.61f, 4.079f};`

Vanilla's own solo-seat local offset (`localFrontHorseRidePos` in `d_a_hozelda.cpp`), kept here so it can be blended against during a rear (and reused as-is otherwise) -- outside of a rear this is exactly what vanilla already uses, so blending starts and ends at the same place vanilla would have placed her.

## Lines 632-635

Precedes: `static const Vec kLinkHorseRidePos = {-68.208984f, 41.609924f, 0.883789f};`

`daAlink_c`'s own local ride offset for Link's solo riding position (`l_localHorseRidePos` in `d_a_alink.cpp`, a file-local constant we can't reference directly). Blending toward it in place of Zelda's own, further-forward/higher offset is the whole point of the rear fix: it's the offset Link's own riding position already proves doesn't clip or float during a rear.

## Lines 638-640

Precedes: `static const Vec kZeldaDualRidePos = {-5.894f, 52.61f, 4.079f};`

Vanilla's own dual-ride (Link also riding) local offset (`localHorseRidePos` in `d_a_hozelda.cpp`) -- the rear seat behind Link, used only to rebuild the cutscene-rotation matrix below; never blended, since the rear-blend fix doesn't apply to this seat.

## Lines 652-654

Precedes: `zelda->shape_angle = horse->shape_angle;`

Kept in sync for other systems that read her logical transform (e.g. the horse-rider-flag clearing and reins handling in `mod_update()`/`on_hozelda_execute_post`), even though the drawn matrix itself is no longer reconstructed from these during a cutscene (see below).

## Lines 659-688

Precedes: `mDoMtx_stack_c::copy(horse->getRootMtx());`

Several of Epona's scripted cutscene "tool demo" modes (`daHorse_c::procToolDemo()` in `d_a_horse.cpp`) drive her purely through a baked animation root-motion track on her own model's joint 0, *without* ever touching `shape_angle` at all -- the demo data's rotate channel (`dDemo_actor_c::checkEnable(ENABLE_ROTATE_e)`) is frequently left unset for such clips, relying entirely on the animation itself to turn her body. `horse->shape_angle` then simply stays frozen at whatever heading she had before the cutscene started, even while Epona's own drawn body visibly swivels via that animation -- exactly the "doesn't face forward" bug, since a `shape_angle`-only copy (as used below for ordinary riding) can't see or reproduce animation-only root motion that never touches `shape_angle` at all.

`horse->getRootMtx()` (her own model's joint 0, read fresh after Epona's own `calc()` for this frame already ran) *does* reflect that root motion -- it's the actual matrix Epona herself is drawn with. So instead of reconstructing Zelda's drawn matrix from the `shape_angle` Euler angles (which may be stale), her whole base matrix is built directly from that same joint matrix plus her local seat offset, rigidly gluing her orientation to Epona's own drawn body for every cutscene frame, baked root motion or not. This mirrors the technique already used for her *position* above (and vanilla's own, for ordinary riding): a local offset carried through `horse->getRootMtx()`, just keeping the matrix's rotation part intact instead of discarding it in favor of a separately-reconstructed one.

Epona's joint-0 matrix itself isn't oriented the same way `shape_angle`'s Y axis treats "facing forward" -- built straight from the model's own root joint, it's rotated a fixed 90 degrees clockwise relative to that convention (Zelda's seat position above was already correct through this same matrix, since translation isn't affected by that discrepancy, but her orientation visibly came out facing off Epona's right side instead of forward). Correcting for it here, as a local Y rotation applied *after* the translate above (so it turns her in place around her already-correctly-placed seat position rather than moving her), rotates her the needed 90 degrees counter-clockwise to align with Epona's actual forward direction.

## Lines 700-735

Precedes: `static const f32 kAutoTargetRange = 4000.0f;`

--- Auto-targeting nearby enemies with Light Arrows ---------------------------------------

Vanilla's `daHoZelda_c` (the horseback-Zelda actor) only ever draws her bow at one specific actor: `mGndAcKeep` is populated exclusively by `daHoZelda_searchGanon()`, a search filter hardcoded to Ganondorf (`fpcNm_B_GND_e`). `execute()` searches for him once per tick whenever `mGndAcKeep` is currently empty; `setAnm()` then only ever enters her bow-draw state machine (`mBowMode`) while the *player's own* Z-target list is locked onto that same actor and `b_gnd_class::checkPiyo()` (a Ganondorf-only "is he currently staggered" accessor) says he isn't; `searchBodyAngle()` (called right after `setAnm()` by `execute()`) aims at whatever `mGndAcKeep.getActor()` happens to be, generically, via its `eyePos` -- it has no Ganondorf-specific logic at all.

So two changes make her auto-fire at nearby enemies instead of just Ganondorf: 1. A pre-hook on `execute()` feeds our own nearest-enemy search into `mGndAcKeep` in place of Ganondorf, scoped to our own spawned Zelda only. Since `execute()`'s own `mGndAcKeep.setActor()` (right after this pre-hook runs) only *validates* whatever actor `mGndAcKeep` already holds by ID -- it doesn't clear a still-alive actor -- our target survives that call, and vanilla's own Ganondorf search is skipped entirely (it only runs when `mGndAcKeep.getActor() == NULL`). `searchBodyAngle()` then aims at our target with no further changes needed. 2. A replace hook on `setAnm()` swaps out the player-lock-on and `checkPiyo()` requirements (meaningless -- and, for `checkPiyo()`, unsafe to even call on a non-Ganondorf actor, since it reads a `b_gnd_class`-specific field through a cast that would otherwise be reading unrelated memory for any other enemy type) for a generic "does our target exist and have health left" check, and drops the real duel's requirement that Link himself be riding double (`checkHorseRide()`), since this mod's own Zelda rides solo. Every other branch of that function -- the horse-anim-to-Zelda-anim mapping, the upper-body bow draw/nock/release state machine, the arrow actor itself -- is an unmodified copy of vanilla's own logic, since none of it depends on the target being Ganondorf specifically. Light Arrows already deal normal damage (and use their normal hit effects) against any ordinary enemy as-is: the shared hit-resolution code (`cc_at_check()` in `d_cc_uty.cpp`) only zeroes out an arrow's light-based bonus damage for Ganondorf himself (whose real damage/stagger is handled separately) and Zant; no new damage type or visual effect is needed for this.

Scoped entirely to our own spawned Zelda: the real scripted duel always uses its own, story-placed HoZelda actor, which is left completely untouched by both hooks below.

## Lines 737-739

Precedes: `static const f32 kAutoTargetRange = 4000.0f;`

How far (in game units, matching e.g. `mpHIO->m.bow_end_distance`'s unused 4000.0f default) and within what cone (reusing `mpHIO->m.bow_end_angle`, the same wider "keep firing" cone vanilla already uses once her bow is drawn) our own Zelda will look for a new target.

## Lines 750-769

Precedes: `static bool is_dormant_enemy(fopAc_ac_c* i_actor) {`

Whether `i_actor` is currently dormant/underground and therefore shouldn't be auto-targeted: vanilla itself treats these enemies as not really "present" while in this state (either collision-wise, status-wise, or both), so Light Arrows can't meaningfully hit them anyway.

- Stalhounds (`E_sh`/`e_sh_class`) spend most of the day buried underground, only surfacing for a few in-game hours at night (`e_sh_stop()`'s own `hourOfDay` check in `src/d/actor/d_a_e_sh.cpp`). Its outer state dispatcher (`action()`) only turns on normal attention/targeting (`fopAcM_OnStatus`/`fopAc_AttnFlag_BATTLE_e`) for the above-ground states (appear/move/attack/damage, `field_0x676` 1-3 and 10); the underground "stop" state (`field_0x676 == 0`) and the sink-back-down "disappear" state (`field_0x676 == 5`) both leave it off instead, exactly like a defeated one would be.

Deku Babas and Baba Serpents are *not* handled here: unlike Stalhounds, their true intangibility window doesn't line up cleanly with a single state field (it actually extends a short while past the state transition that leaves "dormant", e.g. Deku Baba's `invulnerabilityTimer` and Baba Serpent's `field_0x69c[3]` both keep their hit/attack collision spheres shoved away for several frames after `action`/`field_0x66e` already reports them as no longer stay/dormant). Rather than keep chasing that exact window, the whole "Baba" family of enemies is instead excluded from auto-targeting outright in `judge_nearest_enemy()` below, dormant or not -- see the comment there for which actors that covers and why.

## Lines 781-784

Precedes: `static void* judge_nearest_enemy(fopAc_ac_c* i_actor, void* i_data) {`

`fopAcIt_Judge()` stops and returns at the first non-NULL result, so to find the *nearest* enemy (rather than just the first one in the actor list) this always returns NULL -- keeping the iteration going over every actor -- and instead threads the closest candidate so far through `i_data`, read back once `fopAcIt_Judge()` itself returns.

## Lines 794-819

Precedes: `return NULL;`

No explicit Ganondorf (`fpcNm_B_GND_e`) exclusion is needed here: `B_gnd` is only ever placed directly by the stage itself, in the one room that hosts the real horseback archery duel -- and `mod_update()`'s `storyHoZeldaActive` check already tears down (and withholds respawning) this mod's own Zelda for as long as that duel's own story-placed HoZelda exists, so `judge_nearest_enemy()` never runs at all while a `B_gnd` actor could possibly be alive to be found here.

E_WB (Bullbo, the wild boar Bulblins ride) is excluded too: it's its own `fopAc_ENEMY_e` actor separate from its Bulblin rider, but it never attacks on its own -- only the rider does -- so for this mod's purposes it isn't a hostile target.

The entire "Baba" family is excluded outright, dormant or not: E_DB (Deku Baba), E_YD (Twilight Deku Baba), E_HB (Hebi Baba, i.e. Baba Serpent), E_YH (Twilight Hebi Baba) and E_GB (Giant Baba). An earlier version of this exclusion list used `fpcNm_E_YD_e` under the mistaken belief it was Baba Serpent -- per `d_stage.cpp`'s own stage object name table and these actors' class doc comments, `E_yd`/`e_yd_class` is actually "Twilight Deku Baba", while the real Baba Serpent is `E_hb`/`e_hb_class` (with `E_yh`/`e_yh_class` as its own Twilight counterpart) -- so the real Baba Serpent was never actually excluded until now. All five share the same Deku-Baba-like retract/intangible-while-dormant design (see `is_dormant_enemy()`'s comment above), so none of them are worth chasing precise per-state timing for: Zelda simply never auto-targets any of them, dormant or not -- she can still hit them incidentally if the player leads her into melee range, same as before this mod existed.

Dormant/underground enemies (see `is_dormant_enemy()` above) are excluded too: Zelda would otherwise auto-target Stalhounds that haven't surfaced for the night yet.

## Line 847

Precedes: `DEFINE_HOOK(&daHoZelda_c::execute, HoZeldaExecute);`

Pre-hook for change (1) above: see the overview comment further up.

## Lines 856-872

Precedes: `daHorse_c* horse = dComIfGp_getHorseActor();`

While a scripted cutscene is puppeting Epona (`checkHorseDemoMode()`, the same check `mod_update()` uses to decide whether to hide this mod's own Zelda at all -- see `inScriptedCutscene` there), she shouldn't keep drawing her bow and firing Light Arrows at anything, whether she's currently visible or hidden (shrunk to an imperceptible size) for that same cutscene. Visible, her firing arrows mid-cutscene would be a jarring, obviously unscripted interruption the player never asked for; hidden, the player can't even see her doing it, but nearby enemies (and NPCs, e.g. during an otherwise unrelated conversation with Midna) could still visibly react to arrows seemingly coming from nowhere. Both cases are treated exactly like the toggle being off: drop whatever target we'd previously picked so the function below naturally falls back to doing nothing, the same as if she'd never found anyone.

The opening title screen is deliberately excluded from this check (`!is_title_screen()`, mirroring `mod_update()`'s own exclusion): it drives Epona through the exact same `checkHorseDemoMode()` machinery as a real cutscene, but this mod's Zelda is always shown there regardless of the "Show Zelda during cutscenes" toggle, so there's no reason to suppress her combat behavior there either.

## Lines 884-903

Precedes: `static void on_hozelda_execute_post(ModContext*, void* args, void*, void*) {`

Post-hook: while this mod's own spawned Zelda is hidden for a cutscene (`setMatrix()`, already run earlier this same `execute()` via the hook above, shrinks her model down to `kHiddenScale`), `daHoZelda_c::execute()` itself still unconditionally ends with `horse->setReinPosHand(6)` whenever she's riding solo (`mIsSingleRide`) and Link isn't mounted -- attaching Epona's reins to her hand joint (`getRightFingerMtx()`) regardless of whether she's currently visible. Shrinking her model collapses that joint down toward her own seated position rather than moving it away to nothing, so the reins end up visibly anchored at (approximately) where she's sitting instead of following her actual hand -- exactly as conspicuous as not hiding the reins at all.

Once `execute()` returns, if she's currently hidden, this re-derives the reins' position from `daHorse_c::setReinPosNormal()` instead -- the same saddle-anchored position (`m_model`'s own joint 0x15) the reins already default to whenever nobody is riding solo at all. That function itself skips doing anything when `getZeldaActor()` is attached and riding solo (deferring to the hand-based logic instead, by design): since our Zelda is still attached at this point (`execute()` just reattached her, as it does every tick regardless of hiding), she's briefly detached (`setZeldaActor(nullptr)`) around this one call so `setReinPosNormal()` actually recomputes the reins. This has no lasting effect -- `execute()` unconditionally reattaches her (`horse->setZeldaActor(this)`) at the very start of next tick's call anyway, the same trick already used for the grass-whistle fix above.

## Lines 915-917

Precedes: `if (horse->getZeldaActor() == static_cast<fopAc_ac_c*>(zelda)) {`

`zelda` is already known to be alive here (it's the actor whose own `execute()` we're a post-hook of), so a plain pointer comparison against the horse's attached actor is enough -- no need to dereference whatever `getZeldaActor()` returns to find out whose it is.

## Lines 924-929

Precedes: `static constexpr u8 kArrowWaitFrames = 5;`

Vanilla gates each Light Arrow shot behind two scripted 30-frame (0.5s) pauses in the bow draw/ready/shoot/recover state machine below: one between nocking the arrow and it becoming ready to fire, and one between a shot landing and the next draw starting. Shortening both speeds up our spawned Zelda's rate of fire, per the mod's design goal, without touching the draw/shoot animation clips themselves. Set much lower than half (30 -> 5) for testing whether a more aggressive cut is actually noticeable in-game.

## Lines 932-938

Precedes: `static constexpr f32 kDrawAnimSpeedMultiplier = 2.0f;`

With the scripted wait above shortened, the body/bow "ARELORDH"/"BARELORDH" draw animation (`mUpperAnmID == 8`, entered via `setUpperAnime(8)` + `setBowBck(0xB)` below) became the new bottleneck: vanilla always plays it back at its authored speed (`J3DFrameCtrl` rate 1.0, hardcoded inside `daHoZelda_c::setUpperAnime()`/`setBowBck()`) and only advances to the next state once it finishes (`mFrameCtrl[2].checkAnmEnd()`). Doubling both the body clip's frame-controller rate and the bow model's own matching draw clip rate halves the time that state takes, keeping the two in sync with each other.

## Lines 941-946

Precedes: `static void set_anm_auto_target(daHoZelda_c* zelda) {`

Replace hook for change (2) above: a copy of vanilla's `daHoZelda_c::setAnm()` (dusklight's `src/d/actor/d_a_hozelda.cpp`) for our own spawned Zelda only, with just the Ganondorf-specific targeting checks replaced (search the body for "deviates from vanilla" below), plus the faster-rate-of-fire change documented above (search for "kArrowWaitFrames"); every other branch is unmodified. Operates on `zelda->member` throughout, rather than `this->member`, since it isn't itself a member function.

## Lines 972-976

Precedes: `*anm_p = 0xE;`

EXPERIMENTAL (for testing only): vanilla keeps RUN_DASH (0x11) mapped to its own Zelda anim (0xF), which is never paired with the bow pose, so Zelda could only ever draw her bow at a full gallop (RUN_FAST/RUN_SLOW). Folding dash into the same bucket as gallop (0xE) makes the bow-draw gate below reachable while dashing too, so the user can play-test how it looks/feels at more than one gait.

## Line 985

Precedes: `fopAc_ac_c* target_actor = zelda->mGndAcKeep.getActor();`

--- Deviates from vanilla from here... ---

## Lines 988-991

Precedes: `b_gnd_class* ganondorf = (target_actor != NULL && fopAcM_GetName(target_actor) == fpcNm_B_GND_e)`

Only non-NULL if the target this mod picked happens to actually be Ganondorf (never the case in practice: this mod's own Zelda is never alive at the same time as a `B_gnd` actor, see `judge_nearest_enemy()`'s comment above), kept so his own mount/vulnerability checks below still apply correctly rather than silently skipping them if it ever does happen.

## Lines 1001-1004

Precedes: `bool target_vulnerable = false;`

Vanilla requires `b_gnd_class::checkPiyo()` (only meaningful, and only safe to call, on an actual Ganondorf actor) and the player's own Z-target list to equal the target. Replaced here with a generic "does it still have health left" check and an always-on auto-lock, since this mod's Zelda has no player of her own to lock on for her.

## Lines 1012-1017

Precedes: `if ((anm_idx[0] == 0xE || anm_idx[0] == 0x1C) && zelda->field_0x6da == 0 && !zelda->mDamageInit &&`

EXPERIMENTAL (for testing only): vanilla only ever reaches this point with anm_idx[0] == 0xE (full gallop) or 0x1C (the catch-all bucket for every other gait the mapping above doesn't special-case: walk, trot/turn, idle/wait, excitement, etc. -- see the mapping loop above). Both values are safe to allow here since the overlay-blend branch further below (the `anm_idx[0] != 0x1C && anm_idx[0] != 0xE` check) already treats them identically to gallop, so this just widens which gaits can trigger the bow-draw state machine for play-testing.

## Line 1027

Precedes: `int sp28 = 1;`

--- ...to here; everything below is an unmodified copy of vanilla. ---

## Lines 1116-1119

Precedes: `if (anm_idx[2] == 8) {`

Speed up the draw animation itself (see `kDrawAnimSpeedMultiplier` above): both the body's upper-anim frame controller and the bow model's own draw clip need the same rate bump, or the bow's string-pull would keep playing at normal speed after the body's arms already finished drawing.

## Lines 1189-1207

Precedes: `DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);`

The Light Arrow actor (`daArrow_c`) never actually aims itself at Ganondorf, or at anything: `arrowShooting()` only overrides its flight angle/position when the *player* is actively using their own bow-camera aim (`checkBowCameraArrowPosP()`), which is never the case for either the real duel (the player presses the action button while locked on; Link never equips/aims his own bow there) or for the mod's auto-firing Zelda. With that check never satisfied, the arrow simply keeps flying in whatever direction Zelda's hand was last facing per-frame via `setKeepMatrix()` right up until release -- which itself only ever turns a few degrees off of her forward-facing body thanks to `searchBodyAngle()`'s very narrow `bow_search_y_angle` clamp (a handful of degrees). None of this was ever a problem for the real duel, since Ganondorf is always choreographed to stay almost directly ahead of her the whole time; it just never had to aim any wider than that. Once real, potentially off-to-the-side enemies are in play, that narrow a correction isn't enough, and arrows end up flying essentially straight ahead regardless of where the target actually is.

Rather than trying to widen that narrow vanilla clamp (which also drives her visible body/neck twist, and isn't under this mod's control without its own replace hook on `searchBodyAngle()`), this re-aims the arrow directly at the moment it's released: for arrows fired by our own Zelda, the flight direction/speed vanilla would have left untouched is instead pointed straight at whatever enemy she's currently tracking, using the same generic hit-resolution/visuals every other arrow already uses -- only the direction changes.

## Lines 1231-1234

Precedes: `arrow->current.angle.x = -dir.atan2sY_XZ();`

Matches the sign/axis conventions vanilla's own `arrowShooting()` and `searchBodyAngle()` already use elsewhere in this file for converting a world-space direction into the engine's pitch/yaw angle pair, so the arrow's visible orientation while flying stays consistent with every other arrow.

## Lines 1243-1267

Precedes: `DEFINE_HOOK(&cc_at_check, CcAtCheck);`

--- One-hit-kill Light Arrows ---------------------------------------------------------------

Vanilla's own `cc_at_check()` (`d_cc_uty.cpp`) already forces a flat 100 attack power for any hit whose collider material is `dCcD_MTRL_LIGHT` (i.e. any Light Arrow) against any ordinary enemy -- it only leaves Ganondorf (whose real duel damage/stagger is handled entirely separately) and Zant (deliberately left on normal, lower arrow damage) on their own un-boosted power. But a flat 100 power on its own still isn't a guaranteed kill: plenty of enemies simply start with more than 100 health, so they still take several Light Arrow hits in a row even though every single one of those hits already gets that same forced 100-power override.

Rather than trying to special-case (or guess at) every individual enemy's own health-handling quirks -- several reuse their `health` field for non-damage bookkeeping elsewhere in their own state machines, so blindly zeroing it unconditionally would risk stepping on that -- this leaves vanilla's own forced-100-power subtraction completely unmodified and only clamps the *target's current health* down to within that guaranteed-lethal range immediately before the original runs. It's scoped with the exact same Light Arrow material check (and Ganondorf/Zant exemption) the original function itself applies a few lines later, so this never fires for a hit that wouldn't already be getting vanilla's own 100-power override anyway, and never touches health on any frame that isn't a Light Arrow hit at all.

King Bulblin (`e_rdb_class`, `src/d/actor/d_a_e_rdb.cpp`'s `daE_RDB_Create()`, 600-900 health) is additionally exempted here, alongside Ganondorf and Zant: he's a dedicated sub-boss fight (the joust/one-on-one battles), and one-shotting him with a single Light Arrow would trivialize that encounter. Every other enemy -- including ordinary Bulblin riders (`e_rd_class`, 40 health, already a one-shot even without this hook) -- still gets the full one-hit kill.

## Lines 1289-1326

Precedes: `static const u8 kLightArrowResMgrId = 2;`

Zelda's Light Arrow trail/hit mark/charge effects (`daArrow_c::setBlur`/`setLightArrowHitMark`/ `setLightChargeEffect`, particle IDs 0x896E-0x8978) -- plus a number of other effects that turned out to live in the very same archive once this was tested in-game (e.g. whatever produces the arrow's actual visible light trail, which isn't any of those eleven IDs) -- only exist in the horseback-duel stage's room-specific particle archive (`/res/Particle/Pscene181.jpc`) today, not in the always-resident common bank (`/res/Particle/common.jpc`), so they're invisible whenever this mod's spawned-in Zelda fires Light Arrows anywhere outside that one vanilla cutscene. Since the mod can't ship a modified `common.jpc` (that would mean redistributing edited copyrighted game assets), the fix instead teaches the engine to keep `Pscene181.jpc` loaded as a *third*, always-resident particle bank -- alongside `common.jpc` (bank 0) and whatever per-room `Pscene###.jpc` is normally loaded (bank 1) -- and redirects every particle ID that archive actually contains to it. Rather than maintaining a hand-picked allowlist of IDs (which turned out to be incomplete and a maintenance burden -- Zelda's effects aren't the only things in that archive, but anything else in it is simply never requested outside the real duel, so redirecting unconditionally is harmless), `getRM_ID()` below queries bank 2's own `JPAResourceManager` directly via `JPAResourceManager::getResource()`.

`dPa_control_c::mEmitterMng` (a `JPAEmitterManager`) only ever provisions 2 resource-manager slots (`ridMax`, hardcoded at construction in `dPa_control_c::createCommon()`), so bank 2 has to be made to exist first: 1. Grow `ridMax` from 2 to 3 right after `mEmitterMng` is constructed, by hooking `JPAEmitterManager::entryResourceManager()` (a normal, addressable member function) and reallocating `pResMgrAry` with room for bank 2 the moment that method is first called (always with `resMgrID == 0`, registering bank 0, immediately after construction in `createCommon()`). An earlier attempt hooked the constructor itself instead, by symbol name (constructors have no pointer-to-member-function form in C++); that only ever worked by accident; see the comment above `ParticleEntryResourceManager` below for why it was replaced. 2. Once `createCommon()` finishes setting up bank 0, synchronously load `Pscene181.jpc`'s raw bytes into a small dedicated heap of this mod's own (see `s_lightArrowHeap` below) -- *not* `dPa_control_c`'s own resident particle heap; see that variable's comment for why. 3. Build a `JPAResourceManager` over it and register it as bank 2, right there in the same function -- see the comment above `on_particle_create_common_post` for why this load isn't kicked off asynchronously the way vanilla's own per-room scene loads are. 4. Finally, `dPa_control_c::getRM_ID()` (which maps a particle ID to the bank it lives in) is replaced so that, once bank 2 is ready, any particle ID that `Pscene181.jpc`'s own resource manager actually contains resolves to it; everything else keeps using vanilla's existing bank 0/1 logic unchanged.

## Lines 1332-1417

Precedes: `static const u32 kLightArrowHeapSize = 0x200000;  // 2 MiB for Pscene181.jpc and its resource`

`Pscene181.jpc`'s raw archive bytes and its `JPAResourceManager` need to live *somewhere* for the entire game session, but NOT in `dPa_control_c`'s own resident particle heap (`m_resHeap`): that heap is itself a fixed-size `JKRExpHeap` carved out of `mDoExt_getArchiveHeap()` (`d_particle.cpp`: `JKRCreateExpHeap(heapSize, mDoExt_getArchiveHeap(), false)`), and `mDoExt_getArchiveHeap()` is a tightly budgeted heap shared with ordinary per-scene stage resource loading (room archives, camera data, event lists, and so on -- see `mDoExt_getSafeArchiveHeapSize()` and its users in `d_s_play.cpp`). A first attempt grew `m_resHeap` directly (by hooking `JKRExpHeap::create` and bumping the size of just that one allocation by a safety margin), which did make room for `Pscene181.jpc` itself, but permanently shrank `mDoExt_getArchiveHeap()`'s own remaining budget for everything else by that same margin -- every byte added here came out of stage loading's budget. That regressed into a `JKRExpHeap` allocation-failure `SIGABRT` while loading an unrelated new scene (crash log in `res/`), since that scene's stage resources no longer fit in the now-smaller remaining budget.

Instead, this archive gets its own small, completely separate heap (`s_lightArrowHeap`). A second attempt carved that heap's backing memory out of `mDoExt_getZeldaHeap()` via the normal `JKRExpHeap::create(size, parent, errorFlag)` overload (the Zelda heap being the engine's own established "has the most slack" heap -- at boot it's sized to whatever memory is left over after every other heap has already claimed its fixed budget, and it's the heap vanilla's own `mDoDvdThd_mountArchive_c::execute` retries into as a last resort on every platform when every other heap fails to allocate). That still crashed, twice, for two different reasons:

1. On every build target this SDK supports other than the original consoles (`#if TARGET_PC`, i.e. every platform this mod actually ships for), `JKRExpHeap::do_alloc()` treats *any* allocation failure on *any* `JKRExpHeap` as unconditionally fatal and aborts the whole process -- regardless of the `errorFlag` passed to that heap's own constructor (that flag only matters on the non-PC path). That includes the implicit allocation `JKRExpHeap::create(size, parent, errorFlag)` itself makes from its *parent* heap to back the new heap's own memory: if the parent doesn't happen to have `kLightArrowHeapSize` free at that exact moment, `create()` doesn't return `nullptr` the way its signature suggests; it crashes immediately, inside the parent's own `do_alloc()`, before `create()` ever gets a chance to return. 2. A `getMaxAllocatableSize()` pre-check (which cannot itself fail) was added to gate that call and avoid attempting an allocation already known not to fit -- but the crash recurred anyway, still during `dScnLogo_c`'s very first few frames before even the title screen, with no trace of this mod's own diagnostic log lines (crash logs in `res/`). Whatever room the pre-check measured evidently didn't hold by the time `create()` actually ran: other mods load their own boot-time resources into shared heaps during this same narrow window, and nothing stops something else from claiming that same space in between the check and the allocation it was guarding.

Chasing a *third* boot-time memory-pressure failure mode in the same shared heap wasn't worth it: this heap's backing memory now comes from `std::malloc()` instead, bypassing the JKR allocation system -- and every heap it manages -- entirely. `JKRExpHeap` has a second `create()` overload, `create(void* ptr, u32 size, JKRHeap* parent, bool errorFlag)`, that wraps caller-provided memory as a heap's storage directly, with no call to `JKRAllocFromHeap()` on `parent` at all (that parameter is used only to register this heap as a child in the engine's heap tree for bookkeeping/diagnostics, e.g. heap-dump tooling -- it's never range-checked against `ptr`, so `mDoExt_getZeldaHeap()` is passed here purely as a plausible place to be listed, not as a source of memory). `std::malloc()` is ordinary host-side allocation, entirely independent of every one of the game's own fixed-budget heaps (archive, game, Zelda, J2D, command): if the process is so low on memory that it fails, it simply returns `nullptr`, exactly like any other `std::malloc()` call elsewhere in this codebase, with no risk of the `CRASH()` this whole saga was trying to avoid. The one caveat: `JKRExpHeap::do_destroy()` only frees backing memory it allocated itself (the `size`-only `create()` overload); for this pointer-based overload, it just destructs in place and leaves the memory alone, so the raw backing pointer (`s_lightArrowHeapBacking` below) has to be freed with a matching `std::free()` call, by hand, after every `s_lightArrowHeap->destroy()`.

The diagnostic free-size log below (`on_particle_create_common_post`) measured the archive + its resource manager using about 103 KB on the platforms this was tested on -- but the crash chain documented above `s_lightArrowHeap` kept recurring, in the same spot, even after every other heap this feature used to depend on (the shared archive heap, the Zelda heap, the DVD command heap) was removed from its load path one by one, on Windows specifically, while Android and Linux builds of the exact same code never showed any problem. Taken together with the fact that none of this function's own `mods::log::warn()` failure messages below have *ever* appeared in a single crash log across this entire saga, the likely explanation is that this 256 KiB budget itself was simply too tight on Windows (where identical source can still produce different struct layouts/alignment and therefore different real memory usage for the same archive, under a different compiler/ABI than the Linux and Android builds) -- and a too-tight `JKRExpHeap` doesn't *fail gracefully* the way this function's own `nullptr` checks assume: every allocation out of it (including the one inside `JKRDvdToMainRam()` and the one building the `JPAResourceManager` itself) still goes through the same unconditionally-fatal `JKRExpHeap::do_alloc()` described above, which aborts the whole process *before* ever returning `nullptr` to this code. In other words, this heap being our own and fully private only fixed *where* an allocation failure could no longer come from (every other heap in the game); it never made a failure *survivable* in the first place -- there's no such thing as "Light Arrow particles unavailable" if this heap itself runs out of room, only a hard crash.

Since this heap's backing memory is ordinary host-process `std::malloc()`, entirely outside of and invisible to every one of the game's own fixed memory budgets, there is no longer any reason to keep it small the way the two heaps before it had to be (every byte here costs nothing to any other system, mod, or platform) -- so instead of continuing to guess at an exact byte count that happens to survive on every compiler/ABI this mod ships for, it's sized with a large, deliberately wasteful safety margin well beyond anything a ~200 KB archive plus its resource manager should plausibly need on any platform.

## Lines 1418-1419

Precedes: `static void* s_lightArrowHeapBacking = nullptr;`

2 MiB for Pscene181.jpc and its resource manager, allocated from outside any JKR heap

## Lines 1424-1450

Precedes: `DEFINE_HOOK(&JPAEmitterManager::entryResourceManager, ParticleEntryResourceManager);`

With `s_lightArrowHeap` itself no longer dependent on any shared heap having room, the crash still recurred a third time, in the same spot, right after this heap's creation stopped being able to fail that way -- pointing at the *next* thing this code did: kicking off the archive load via `mDoDvdThd_toMainRam_c::create()`, mirroring how vanilla's own `readScene()` loads every per-room scene archive. That call allocates its own small command object from `mDoExt_getCommandHeap()` (`m_Do_dvd_thread.cpp`) -- a single, tiny (a few KiB), engine-wide `JKRExpHeap` shared by *every* in-flight DVD read the whole game and every active mod issues, nowhere near as generously sized as the multi-megabyte heaps above. Vanilla itself relies on this same heap to load `common.jpc` at this exact boot moment (`d_s_logo.cpp`), so it isn't normally a bottleneck in a vanilla, single-mod-free boot -- but it's still a *shared* `JKRExpHeap`, subject to the exact same TARGET_PC unconditional-abort-on-failure behavior described above, and this mod's own additional load is one more simultaneous claim against it during the single busiest, most contested moment of the entire game session, where other active mods are independently loading their own boot-time resources too.

Rather than adding a fourth heap-sizing workaround to a heap this mod doesn't own and can't resize, this load bypasses the asynchronous command-queue system entirely: `JKRDvdToMainRam()` (`JKRDvdRipper.h`) is the same low-level, synchronous DVD-read routine `mDoDvdThd_toMainRam_c` itself calls on the DVD thread, and it's already used directly, synchronously, by plenty of other vanilla code (`JKRDvdArchive`, `JKRCompArchive`, `JKRAramArchive` constructors) with no command object and no command heap involved at all -- it reads the file into a buffer it allocates directly from whatever heap is passed to it (`JKRAllocFromHeap(heap, ...)`), which here is `s_lightArrowHeap`: this mod's own private, `std::malloc`-backed heap, fully isolated from every other heap in the game and every other mod. Calling it directly, synchronously, right here in `on_particle_create_common_post` (rather than polling an async command every frame, the way `poll_light_arrow_particle_bank()` used to) means this entire feature's load path now has zero remaining dependency on any heap this mod doesn't fully own and control itself.

## Lines 1452-1471

Precedes: `DEFINE_HOOK(&JPAEmitterManager::entryResourceManager, ParticleEntryResourceManager);`

Named hooks on `JPAEmitterManager`'s constructor turned out to be fragile across platforms: constructors have no pointer-to-member-function form in C++, so the only way to target one via `DEFINE_HOOK_SYMBOL` is by name. A first attempt hardcoded the Itanium-mangled symbol (`_ZN17JPAEmitterManagerC1EjjP7JKRHeaphh`), which only exists in the symbol manifest for build targets using the Itanium ABI and failed to resolve on Windows x86_64 ("symbol ... not found"). Switching to the unmangled qualified display name (`"JPAEmitterManager::JPAEmitterManager"`) fixed that, but traded it for two worse problems: it failed to *build* on AppleClang arm64 (macOS and iOS), and crashed on *initial load* on Windows x86_64 -- a named hook on a constructor is simply not a portable, well-supported pattern across the SDK's compilers.

Instead, this hooks `JPAEmitterManager::entryResourceManager()` -- an ordinary non-virtual member function, addressable the normal way with `&Class::method`, no name resolution involved at all. `dPa_control_c::createCommon()` always calls it immediately after constructing `mEmitterMng`, registering bank 0 with `resMgrID == 0` -- the first and only call made with that ID. At that exact moment `pResMgrAry` already exists (built by the constructor with its original, too-small `ridMax`) but hasn't been touched yet, and -- critically -- the solid heap it was allocated from (`dPa_control_c::mHeap`) hasn't been trimmed to size yet either: that only happens via `mDoExt_adjustSolidHeap()` at the very end of `createCommon()`, well after every `entryResourceManager()` call it makes. So there's still room to grow `pResMgrAry` here, exactly as if the constructor itself had been asked to allocate a bigger one.

## Lines 1496-1512

Precedes: `DEFINE_HOOK(&dPa_control_c::createCommon, ParticleCreateCommon);`

Loads `Pscene181.jpc`'s raw bytes right after vanilla finishes setting up the common bank (and, critically, after `mEmitterMng` -- constructed earlier in this same function -- has already had its slot count bumped by the hook above), then immediately builds a `JPAResourceManager` over it and registers it as bank 2, all synchronously, right here, rather than kicking off an asynchronous load and polling for it to finish on a later frame (see the comment above this load for why).

`createCommon()` is NOT a true one-shot, console-boot-only call: it's only ever invoked from one call site (`dScnLogo_c`'s boot-time phase), but Twilight Princess implements several "big" scene transitions (dying and choosing to continue, certain major stage-to-stage crossings) as a full software reset (`mDoRst`) that tears down and rebuilds the entire particle system from scratch -- a brand new `dPa_control_c`, a brand new resident heap, and a brand new `JPAEmitterManager` with an empty bank 2 slot -- which re-enters this very function. The mod's own static state below lives outside that reset, so it must be unconditionally re-armed here every time this runs, rather than treated as a permanent latch; otherwise, after the first such reset, `s_lightArrowJpcReady` stays stuck `true` forever even though the new `JPAEmitterManager` never actually got bank 2 registered, silently breaking every Light Arrow hit mark/charge effect from that point on.

## Lines 1517-1518

Precedes: `s_lightArrowResMgr = nullptr;`

The previous resource manager (if any) belonged to the heap being destroyed/recreated below, so this pointer must not be dereferenced until a new one is built.

## Lines 1521-1525

Precedes: `if (s_lightArrowHeap != nullptr) {`

`s_lightArrowHeap` is this mod's own heap (see its declaration above), independent of `dPa_control_c`'s lifecycle, but its *contents* (the previous load's archive bytes and resource manager, if any) belong to a bank mapping that's no longer valid once this runs again -- so it's destroyed and recreated fresh here, exactly like vanilla's own resident particle heap is, rather than reused in place.

## Lines 1530-1532

Precedes: `if (s_lightArrowHeapBacking != nullptr) {`

The pointer-based `JKRExpHeap::create()` overload used below doesn't free this itself (see the comment above this function) -- it has to be freed by hand, every time, before the next `std::malloc()` call replaces it.

## Lines 1538-1539

Precedes: `s_lightArrowHeapBacking = std::malloc(kLightArrowHeapSize);`

Ordinary host-side allocation, entirely outside the JKR heap system: on failure this simply returns `nullptr`, with none of the abort-on-failure behavior described above this function.

## Lines 1557-1562

Precedes: `u32 jpcSize = 0;`

Synchronous, direct DVD read: the same low-level routine `mDoDvdThd_toMainRam_c` itself calls on the DVD thread, but invoked here directly, with no command object and no command heap involved at all (see the comment above `s_lightArrowHeap` for why this replaced the async `mDoDvdThd_toMainRam_c::create()` approach used previously). The destination buffer is allocated straight out of `s_lightArrowHeap` -- this mod's own private heap -- with no other heap touched anywhere in this call.

## Lines 1585-1587

Precedes: `mods::log::info("Pscene181.jpc loaded as particle bank {}, resource heap has {} bytes free",`

Logged once per load (including after every soft-reset reload) so this can be checked against `kLightArrowHeapSize` above without needing a debugger: if this ever trends towards 0, the heap needs to grow again, the same way it did when it was first sized.

## Lines 1592-1594

Precedes: `DEFINE_HOOK(&dPa_control_c::getRM_ID, ParticleGetRmId);`

Once bank 2 is ready, redirects any particle ID that `Pscene181.jpc`'s own resource manager actually contains to it; everything else (and these same IDs, while bank 2 is still loading) keeps using vanilla's existing top-bit common/scene selection unchanged.

## Lines 1609-1616

Precedes: `static ModResult build_mods_panel(ModContext*, UiElementHandle panel, void*, ModError*) {`

Rather than attempting to patch up Zelda's orientation, animation, and any overlapping props for every individual story cutscene that drives Epona (a reaction/firewood/etc. fix was tried for each as they were found, but kept surfacing new, similarly-themed regressions with no end in sight), this mod instead hides its own spawned Zelda for the duration of any such cutscene by default, controlled by `g_cvarShowInCutscenes` above (see `mod_update()`'s cutscene-detection gate). The orientation fix above still applies whenever she *is* shown (including when the user opts in via that toggle), since that's a simple, narrowly-scoped correction to her own seat, not a per-cutscene patch.

## Line 1649

Precedes: `}`

Not fatal: this hook is diagnostics-only.

## Line 1656

Precedes: `}`

Not fatal: the mod still works, just without the horse-call fix.

## Line 1664

Precedes: `}`

Not fatal: the mod still works, just without the cutscene-orientation fix.

## Line 1672

Precedes: `}`

Not fatal: the mod still works, Zelda just won't auto-target enemies.

## Line 1681

Precedes: `}`

Not fatal: the mod still works, just without the hidden-reins fix.

## Line 1690

Precedes: `}`

Not fatal: the mod still works, Zelda just won't auto-target enemies.

## Line 1699

Precedes: `}`

Not fatal: the mod still works, arrows just won't be re-aimed at the target.

## Lines 1708-1709

Precedes: `}`

Not fatal: the mod still works, Light Arrows just keep vanilla's own flat-100-power behavior (still enough to one-shot most ordinary enemies, just not high-health ones).

## Lines 1718-1719

Precedes: `}`

Not fatal: the mod still works, Light Arrows just won't show their hit mark/charge effects outside the one vanilla cutscene that already has them.

## Line 1728

Precedes: `}`

Not fatal: same as above.

## Line 1737

Precedes: `}`

Not fatal: same as above.

## Lines 1749-1750

Precedes: `}`

Not fatal: `show_zelda_in_cutscenes()` already falls back to "off" when the var isn't registered.

## Lines 1762-1763

Precedes: `}`

Not fatal: `auto_target_enemies_enabled()` already falls back to "off" when the var isn't registered.

## Lines 1772-1773

Precedes: `}`

Not fatal: the config var still exists (and can be set via config.json/--cvar even without a UI control for it), it just won't be reachable from the Mods window.

## Lines 1784-1789

Precedes: `remove_spawned_zelda();`

No horse actor is currently loaded (e.g. a different area, or before Epona is tamed). `daHoZelda_c` never deletes itself (it has no `is_delete` method), so if we don't explicitly delete the actor we spawned here it would keep existing and executing forever, orphaned from any horse. That leaked actor is a likely cause of the "two overlapping Zelda models" bug: once a new horse actor appears later, we'd spawn a second HoZelda while the first, orphaned one is still alive and animating.

## Lines 1795-1814

Precedes: `bool horseModelNotDrawn = horse->checkHorseCallWait();`

Right after loading a save made in a different area than Epona was left in, `daHorse_c::create()` sets `FLG0_NO_DRAW_WAIT` on her instead of restoring her saved position (see that function's own stage/room-name comparison against `dComIfGs_getHorseRestartStageName()`/`...RoomNo()`), and both `draw()` and `execute()` return immediately while it's set -- `horse` itself is a fully created, valid actor the whole time (its fields, including `current.pos`/`getRootMtx()`, already hold valid values from `create()`/the model's initial pose, not garbage), it's only her own model/collision that stay undrawn/intangible until the player calls her with the grass whistle (`callHorseSubstance()` clears the flag the moment it's set).

This mod previously deleted (`remove_spawned_zelda()`) and later recreated its own HoZelda actor for the entire duration of this state, exactly like the "no horse loaded" case above. That churn -- tearing down and recreating a `HoZelda` actor purely because Epona's *model* isn't being drawn, even though her actor data is fully loaded the whole time -- is suspected of being a contributing cause of persisting SIGABRTs elsewhere in `HoZelda`'s loading path. Since the horse actor is valid throughout, there's no need to delete our own Zelda here at all: instead keep her loaded (spawning her once, same as ordinary gameplay, if she isn't already) and simply hide her in place via the same scale-based mechanism already used for scripted cutscenes (`HoZeldaSetMatrix`'s `s_hideSpawnedZeldaInCutscene` branch), so she never has to be recreated once the whistle call clears the flag.

## Lines 1817-1818

Precedes: `bool hadSpawnedZeldaBeforeRefresh = s_hasSpawnedZelda;`

Refresh whether the actor we previously spawned is still alive (it may have been deleted by the game for reasons outside our control, e.g. a scene change).

## Lines 1823-1838

Precedes: `if (hadSpawnedZeldaBeforeRefresh && !s_hasSpawnedZelda) {`

Diagnostic: `find_spawned_zelda()` just discovered the actor we spawned was deleted out from under us by the engine itself (its own comment: "e.g. deleted on a room change"), rather than by our own `remove_spawned_zelda()` calls below, which only ever run before this point in the frame via the early `horse == nullptr` return above (`checkHorseCallWait()` no longer deletes her at all). A captured dusklight engine log confirmed one specific cause: switching between two sub-cuts of the same cutscene chain (e.g. "demo01_01" -> "demo01_02") reloads the entire room's actor list including Epona herself, without the room number ever changing. The `inScriptedCutscene` branch further down has a recovery respawn for exactly that case, but -- per a second captured log, this time of a SIGABRT -- blindly respawning her any time she's merely found missing is unsafe: using the horse-call grass whistle to summon Epona across areas goes through the exact same "actor is gone, horseDemoMode is true" shape as the safe sub-cut case, but is a full cross-room teleport, not an in-place reload, and attempting `fopAcM_create` against the old, about-to-be-torn-down room's horse during that transition crashed outright. `s_zeldaLostInSameRoom`, computed here by comparing the room she's lost in against the room she was last confirmed alive in, distinguishes the two: true only for an in-place reload (room unchanged), false for a cross-room loss like the horse-call teleport.

## Lines 1851-1853

Precedes: `if (s_hasSpawnedZelda) {`

Keep tracking the room she's actually in for as long as she's alive, so the comparison above always reflects the room she was in immediately before her most recent loss, not a stale value from further back.

## Lines 1858-1868

Precedes: `fopAc_ac_c* attachedZelda = horse->getZeldaActor();`

Diagnostic: if our tracked actor is still alive but isn't the one currently attached to the horse, something else (the story, or another spawn we lost track of) has its own HoZelda riding at the same time as ours — i.e. exactly the "two overlapping Zelda models" bug. This should never happen given the checks below, but if it does, logging it (with both actors' IDs) is the best lead available for further diagnosis without being able to run the game.

`attachedZelda` is deliberately never dereferenced (e.g. via `fopAcM_GetID`) here -- same reasoning as `is_spawned_zelda_attached()`'s own comment -- since `horse->getZeldaActor()`'s cached pointer isn't guaranteed to have been refreshed for this exact tick yet, and this diagnostic isn't worth risking a crash over; its raw address is enough to tell from the log whether it's null or some other actor without needing to resolve to an ID.

## Lines 1878-1892

Precedes: `bool inScriptedCutscene = horse->checkHorseDemoMode() && !is_title_screen();`

Whether a real, scripted story cutscene -- as opposed to ordinary gameplay -- is currently driving Epona. `daHorse_c::checkHorseDemoMode()` (`field_0x16b8 != 0`) is set by `setDemoData()` any time the horse is being driven by something other than the player -- i.e. a scripted cutscene -- including ordinary non-"tool demo" procs (jump/stop/turn/move/ wait) that a cutscene can puppet her through, not just the single `procToolDemo()` (`PROC_TOOL_DEMO_e`) state. An earlier version of this check only looked at `PROC_TOOL_DEMO_e`, which missed every cutscene that drives Epona via one of those other procs instead, letting this mod's own Zelda incorrectly appear in cutscenes she shouldn't regardless of the "Show Zelda during cutscenes" toggle.

The only case that should always stay visible regardless of that toggle is the opening title screen (`is_title_screen()`, which drives Epona the exact same way a cutscene does -- see that function's own comment). Every other `checkHorseDemoMode()` event -- including the horse-call/grass-whistle gallop-back, NPC conversations, and area/scene transitions -- is treated the same as any other story cutscene and hidden by default.

## Lines 1895-1902

Precedes: `bool storyHoZeldaActive = find_other_hozelda() != nullptr;`

Whether a story-placed HoZelda (never one this mod spawned) currently exists anywhere, e.g. the real horseback archery duel against Ganondorf, where the game itself places and drives its own HoZelda riding solo. This mod's own spawned Zelda would otherwise duplicate her, and -- since this mod's auto-target/combat hooks are scoped to our own actor only -- our own Zelda would keep auto-targeting nearby enemies the entire time, overlapping the real duel's own Ganondorf-only targeting. So this mod's entire functionality simply steps aside for as long as the real duel's own HoZelda exists: our own spawned Zelda is torn down and not respawned until the story's HoZelda is gone again.

## Lines 1909-1925

Precedes: `s_hideSpawnedZeldaInCutscene = !show_zelda_in_cutscenes() || is_always_hidden_cutscene();`

Default behavior: don't intrude on story cutscenes with our own spawned Zelda unless the user opts in via the "Show Zelda during cutscenes" toggle. This only ever affects our own spawned actor -- cutscenes with their own story-placed HoZelda (e.g. the real Ganondorf duel) are already handled above by the `storyHoZeldaActive` check.

Unlike earlier, she is never deleted here: the `HoZeldaSetMatrix` hook instead hides her in place (moved far underground and shrunk to an imperceptible size) for as long as `s_hideSpawnedZeldaInCutscene` stays true, so she's back exactly where she belongs the instant the cutscene ends with no recreation needed. If she doesn't already exist by the time a cutscene starts, she simply stays unspawned for its duration (see below) rather than being created mid-cutscene, which isn't safe (see `spawn_zelda_on_horse()`'s callers' comments).

A few specific cutscenes (`is_always_hidden_cutscene()`) are staged around Epona not visibly carrying a second rider at all, so this mod's Zelda stays hidden through those regardless of the toggle -- she's un-hidden again the instant the cutscene ends, same as any other cutscene, since this whole branch only runs while `inScriptedCutscene`.

## Lines 1928-1957

Precedes: `bool isDemo01_03 =`

Recovery for the "stays hidden for the rest of a multi-part cutscene" bug: a captured dusklight engine log showed that switching between two sub-cuts of the same cutscene chain (e.g. "demo01_01" -> "demo01_02", observed with the room/layer unchanged) tears down and recreates the entire room's actor list -- including both `daHorse_c` itself (visible in that log as a fresh `fpcBs_Create` for `fpcNm_HORSE_e` right at the transition) and our own spawned Zelda riding it -- as part of loading that next sub-cut's demo data. Our own cleanup call for the old, already-destroyed actor runs afterwards and harmlessly no-ops (the engine's own actor service logs "doesn't exist" for it), but because the respawn branch below is gated to `!inScriptedCutscene`, and a chain like this one never leaves `inScriptedCutscene` between sub-cuts, she previously had no way to come back until the entire chain ended, even for sub-cuts that aren't supposed to hide her at all (`is_always_hidden_cutscene()` only covers the specific sub-cuts staged without a second rider -- every other sub-cut in the same chain should still show her once the toggle is on).

A second captured log (this time ending in a SIGABRT) showed this straightforward-looking fix is NOT safe to apply unconditionally: using the horse-call grass whistle to summon Epona from a different area she'd been left in produces the exact same observable shape here (`horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda` while `checkHorseDemoMode()` is true for the horse-call's own gallop-back sequence), but is a full cross-room teleport tearing down and rebuilding an entirely different room, not an in-place same-room reload -- and `spawn_zelda_on_horse()` against the stale, about-to-be-destroyed old room's horse crashed outright. `s_zeldaLostInSameRoom` alone was not actually sufficient to rule out every unsafe case either (the SIGABRTs continued even after that guard was added), so this recovery respawn is now scoped down further, to only the one specific transition it was ever confirmed necessary and safe for: "demo01_02" -> "demo01_03" (the only sub-cut transition in the always-hidden -> normally-shown direction that this mod has actually observed losing her this way). Every other `inScriptedCutscene` transition simply leaves her unspawned for the rest of that cutscene chain instead of risking a respawn against a potentially stale `horse` pointer.

## Lines 1965-1971

Precedes: `s_hideSpawnedZeldaInCutscene = true;`

Epona's own model/collision aren't drawn/tangible yet (see `horseModelNotDrawn`'s own comment above), but `horse` itself is already a fully valid, loaded actor -- so keep our own Zelda loaded here too instead of leaving her unspawned, exactly like the ordinary gameplay case just below, just hidden in place via the same scale-based mechanism used for scripted cutscenes rather than shown. She's already attached to a horse that is itself hidden, so she'll be back at Epona's side the instant the player calls her with the grass whistle and `horseModelNotDrawn` clears, with no recreation needed.

## Lines 1977-1983

Precedes: `s_hideSpawnedZeldaInCutscene = false;`

Keep Zelda riding along on Epona at all times during ordinary gameplay, even after Link dismounts: if nobody (neither the story nor this mod) currently has her attached to the horse, spawn her. `daHoZelda_c::execute()` attaches itself to the current horse every tick (`horse->setZeldaActor(this)`), and `daHoZelda_c::setMatrix()`/`setRideOffset()`/ `setAnm()` already support her sitting alone at the front of the saddle whenever Link isn't riding (the `mIsSingleRide` case), so once created it takes care of the rest on its own: reins, dual-ride animation blending, and the solo idle animation.

## Lines 1990-2005

Precedes: `if (is_spawned_zelda_attached(horse)) {`

`daHoZelda_c::setMatrix()` unconditionally calls `onHorseZelda()` every tick whenever any HoZelda actor rides the current horse, marking Link's `FLG2_HORSE_ZELDA` player flag. The game uses that flag to recognize the scripted horseback duel against Ganondorf, and while it is set, several ordinary systems are deliberately disabled for the duration of that fight: dismounting (`checkSpecialHorseRide()` in `daAlink_c::checkHorseGetOffAndSetDoStatus` suppresses `BUTTON_STATUS_DISMOUNT`) and talking to/checking in with Midna (`daAlink_c::orderZTalk()` bails out early whenever `checkHorseZelda()` is true).

Outside of that real story duel, this mod is the only thing keeping Zelda mounted, so we clear the flag right back every tick that our own actor (never a story-placed one) is the rider: since `setMatrix()` re-asserts it once per frame too, this turns into a one-flag tug-of-war that is won by whichever side runs last before the player's own logic reads it, but because we clear it every single tick, the flag is never left set for more than the single frame in which `daHoZelda_c` re-asserts it, and it is always false again by the time the player's dismount/Midna checks run on the next tick. A real scripted duel always places its own HoZelda actor (never ours), so this never touches the flag during the actual fight.
