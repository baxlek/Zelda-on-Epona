# AI Code Commentation

This file contains every explanatory code comment that was previously inline in
`src/mod.cpp`. The comments were moved here (verbatim) to keep the source file
comment-free; each entry below is headed by the function, declaration, or branch
it documents, in the same order it appears in `src/mod.cpp`, so you can match an
entry back to its original location.

## `epona_follow_active()`

> Follows "Zelda active in combat" so the two toggles move together: Epona stops following the
> instant combat mode is turned off, even if the follow checkbox itself is still checked.

## `s_zeldaBackwardBowTarget`

> True while the spawned Zelda's current bow target sits behind her rather than in front, which
> means she should aim/fire with her body turned 180 degrees from the usual forward-facing pose.
> Only ever set while dual-riding; solo-riding Zelda never fires backwards.

## `on_hozelda_set_matrix_post()` — backward bow-target branch

Above the body-flip logic executed when `dualRide && zelda->mBowMode != 0 && s_zeldaBackwardBowTarget`:

> Firing at a target behind her: turn her whole body 180 degrees from the usual
> forward-facing pose so the normal firing animation points backwards instead. Her seat
> position on the horse (current.pos) is left untouched, only the facing changes.

## `mirror_seen_angle_behind()`

> fopAcM_seenActorAngleY() returns how far (0..0x8000) an actor sits from i_actorA's forward
> facing direction. Mirroring that value around the halfway point (0x8000) yields the
> equivalent "how far from directly behind" angle, letting the same cone-width checks used for
> forward targeting be reused to search for targets behind Zelda.

## `seen_angle_from_yaw()`

> Measures how far i_targetPos sits (0..0x8000) from the i_refAngleY facing direction, as seen
> from i_fromPos. Unlike fopAcM_seenActorAngleY(), the facing angle is passed in explicitly
> instead of being read from an actor's own shape_angle.y -- this keeps targeting decisions based
> on a stable reference even while Zelda's own shape_angle.y is snapped 180 degrees for backward
> firing (reading her own, already-flipped shape_angle.y back as the "forward" direction would
> make her flip-flop between facings every frame).

## `zelda_targeting_reference_angle_y()`

> The yaw Zelda's targeting should be measured against: the horse's facing while dual-riding
> (stable, unaffected by her own backward-firing snap), or her own facing otherwise (e.g.
> solo-riding, where she isn't necessarily aligned with the horse).

## `find_nearest_enemy()`

> i_behind selects targets roughly opposite Zelda's forward facing (for firing backwards while
> dual-riding) instead of the normal forward-facing cone.

## `kAnmHorseStand` / `kAnmHorseExcitement` / `kAnmHorseExcitement2` / `kAnmHorseWait01` / `kAnmHorseLash`

Each constant had a short trailing comment identifying its animation index:

- `kAnmHorseStand = 20` — `ANM_HS_STAND`: rearing "stand" pose
- `kAnmHorseExcitement = 8` — `ANM_HS_EXCITEMENT`: spooked-by-enemy reaction
- `kAnmHorseExcitement2 = 9` — `ANM_HS_EXCITEMENT_2`: same, alternate clip
- `kAnmHorseWait01 = 27` — `ANM_HS_WAIT_01`: normal idle pose
- `kAnmHorseLash = 14` — `ANM_HS_LASH`: neck-toss "startled" reaction

## `set_anm_auto_target()` — `bow_mode_angleY` computation

Above `int bow_mode_angleY = s_zeldaBackwardBowTarget ? mirror_seen_angle_behind(...) : ...;`:

> When the current target is behind Zelda, use the mirrored angle so the same cone-width
> checks below apply to "how far from directly behind" instead of "how far from forward".

## `kEponaFollowDistance` / `kEponaFollowRampRange`

> Standoff distance Epona tries to hold while following Link on foot, and the extra range beyond
> it over which her approach speed ramps up to kEponaFollowMaxStickValue. Set to half the
> clawshot's maximum range (daAlinkHIO_hookshot_c0::m.mMaxLength == 2000.0f in
> d_a_alink_HIO_data.inc) per user request, since our earlier guesses at a sensible "in-game
> units" distance (20, then 200) both turned out to be far too small a fraction of the engine's
> real distance scale.

## `kEponaFollowMaxStickValue`

> Ceiling on the forward stick magnitude we ever feed her while following. Forward speed target
> is normalMaxSpeed * padStickValue^2, and daHorse_c::setMoveAnime() blends into a full running
> "dash burst" animation once that target crosses fastwalk_to_run_rate; combined with riderless
> horses always using the hard/quick m_hio->m.stopping_deceleration rate to slow down (rather
> than a gentle coast), braking from that speed looked like an unintended "skid". Capping below
> 1.0 keeps her at a brisk pace at most, so she never reaches that run animation and a quick stop
> doesn't look like a skid.

## `kEponaFollowMaxDistance`

> Beyond this distance Epona gives up following entirely (as if the feature were off) rather than
> keep trying to close in from arbitrarily far away.

## `kEponaFollowResumeMargin`

> Extra distance beyond a stop threshold Link must put between himself and Epona before she
> resumes an action (approaching, or facing him) once stopped. Without this hysteresis, a bare
> "distXZ <= threshold" flickers once she's close enough to physically touch Link: the collision
> push-back that keeps the two actors from interpenetrating nudges distXZ back above the
> threshold for a tick, re-engaging the action and immediately undoing it, over and over.

## `kEponaFollowStillDistance`

> Distance within which Epona holds completely still -- no walking, no turning -- so Link can
> walk up and mount her without her fidgeting. Between this and kEponaFollowDistance she's still
> close enough that walking isn't warranted, but she keeps turning to face him.

## `s_eponaFollowApproaching`

> Whether Epona is currently in her "approaching" phase, as opposed to stopped/settled within the
> follow distance. See kEponaFollowResumeMargin above for why this needs to be sticky across
> frames rather than recomputed from distance alone each tick.

## `s_eponaFollowFacing`

> Whether Epona is still actively correcting her facing toward Link, as opposed to fully settled
> (holding still) within kEponaFollowStillDistance. Also sticky, for the same reason as above.

## `kEponaFollowIdleTurnMax` / `kEponaFollowIdleTurnMin`

> Per-tick turn-rate clamp used to turn Epona in place to face Link: both while she's stopped
> within the follow distance, and before she's allowed to start walking toward him (see
> kEponaFollowAlignmentCone below).

## `kEponaFollowAlignmentCone`

> How closely Epona must already be facing Link before she's allowed to walk toward him.
> daHorse_c::setSpeedAndAngle() treats any commanded angle more than 0x6000 (135 degrees) away
> from her current facing as a "spin around" input and drives speedF *backward* instead (vanilla's
> player-held-reverse-stick mechanic) -- not what we want for an absolute "walk toward this point"
> follow behavior. Withholding forward stick magnitude until she's within this cone keeps her
> comfortably clear of that branch and matches the requested "turn to face before moving" order.

## `epona_follow_should_engage()`

Above the reset performed when `horse == nullptr || player == nullptr || !epona_follow_active()`:

> Reset so the next engagement starts by approaching and actively facing Link.

Trailing comments on the early-out branches:

- `if (player->checkHorseRide()) { ... return false; }` — Link is still mounted, nothing to follow
- `if (horse->checkHorseDemoMode() || horse->checkHorseCallWait() || dComIfGp_event_runCheck()) { return false; }` — don't interfere with cutscenes, the no-draw wait state, or other scripted events
- `if (horse->checkRodeoMode() || horse->checkStateFlg0(daHorse_c::FLG0_UNK_10000000)) { return false; }` — rodeo minigame, or already mid an in-progress whistle-call run

Above the reset performed when `horse->current.pos.absXZ(player->current.pos) > kEponaFollowMaxDistance`:

> Too far away to reasonably chase down; stop trying until Link closes the gap himself.

## `on_horse_set_stick_data_post()`

> Feeds Epona the same "virtual stick input" real player steering or the whistle-call feature
> would, so her approach reuses the native movement/turning/acceleration/animation/wall-collision
> pipeline instead of fighting it with a hand-rolled position override.

Above `if (!s_eponaFollowApproaching || !aligned) { horse->m_padStickValue = 0.0f; return; }`:

> Either settled within the follow distance, or still turning to face Link (see
> on_horse_follow_execute_post) -- withhold forward stick input either way.

Above `horse->onMoveAccept();`:

> Without a real rider, daHorse_c::procWait() unconditionally zeroes any speed
> setSpeedAndAngle() computes unless ERFLG0_MOVE_ACCEPT is set that tick, so she'd otherwise
> never transition into her walking proc at all. This mirrors what the rodeo minigame's own
> autonomous movement does to drive the horse without a rider (setStickRodeoMove()).

## `on_horse_follow_execute_post()`

> Turns Epona in place to face Link: before she's allowed to start walking toward him (the native
> stick-driven turning in setSpeedAndAngle() only engages while forward stick magnitude is
> nonzero, which we deliberately withhold above until she's already facing roughly the right
> way), and also while she's stopped walking but still further than kEponaFollowStillDistance
> away. Once she's within kEponaFollowStillDistance, this does nothing at all -- she holds fully
> still, including her facing, so Link can approach and mount her without her fidgeting.

Above the `if (!s_eponaFollowFacing)` check:

> s_eponaFollowApproaching/s_eponaFollowFacing were already updated for this tick by
> on_horse_set_stick_data_post, which runs earlier within daHorse_c::execute().

Trailing comments on the two early returns:

- `if (!s_eponaFollowFacing) { return; }` — within the still distance; hold fully still, don't even turn to face Link
- `if (s_eponaFollowApproaching && aligned) { return; }` — already aligned; native turning/movement handles this tick
