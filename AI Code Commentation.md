# AI Code Commentation

This file contains every explanatory code comment that was previously inline in
`src/mod.cpp`. The comments were moved here (verbatim) to keep the source file
comment-free; each entry below is headed by the function, declaration, or branch
it documents, in the same order it appears in `src/mod.cpp`, so you can match an
entry back to its original location.

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

## `hozelda_mouth_joint_callback()` / `kZelhJntLipSync`

> Faux lip-sync for the spawned Zelda's dialogue (see `update_zelda_mouth_animation()` further
> down). Her model has no authored mouth-flap animation of its own, unlike native talking NPCs:
> those rely on `daNpcT_c::ctrlMsgAnm()` (d_a_npc.cpp) toggling playback of an authored "talk"
> BCK/BTP in bursts timed to `dMsgObject_isMouthCheck()`, using per-NPC-class motion-sequence-
> manager infrastructure her class doesn't have. Her skeleton does have dedicated facial joints,
> though -- `ZELH_JNT_CHIN_e = 0x5` and `ZELH_JNT_MOUTH_e = 0xA` in the decompiled `HoZelda.h`
> resource header's `ZELH_JNT` enum (assets/RZDP01/res/Object/HoZelda.h) -- neither of which is
> one of the four joints (`daHoZelda_c::create()`'s `getJointNodePointer(0/1/23/4)`) the native
> code itself registers a callback on, so we register our own on both, reusing the exact matrix
> idiom `daHoZelda_c::modelCallBack()` already applies to its own joints
> (`mNeckAngle`/`mBodyAngle`): copy the joint's current anm matrix, apply an extra local
> rotation, write it back, then mirror it into `J3DSys::mCurrentMtx` for
> `J3DJoint::recursiveCalc()` to pick up.
>
> Driving both joints (rather than `ZELH_JNT_MOUTH_e` alone, as a first attempt did) is a
> deliberate hedge: build #384 testing showed zero visible movement in-game from `MOUTH` alone.
> `ZELH_JNT`'s index ordering is a depth-first joint-tree traversal (parent before children,
> siblings grouped), and `CHIN` (0x5) sits immediately after `HEAD` (0x4) -- where a modeler
> would place the primary jaw/mandible bone -- while `MOUTH` (0xA) only appears after both
> hair-strand chains (`MOMIL1/2`, `MOMIR1/2`, indices 0x6-0x9) are fully traversed, meaning it's
> a later-added sibling of `CHIN` under `HEAD`, not a child of it. That ordering suggests `CHIN`
> is the bone modelers actually skinned the jaw/lower-mouth geometry to, and `MOUTH` may be a
> lighter-weight corner-of-mouth or locator joint with little or no skin influence on its own --
> but this still can't be confirmed without a render pipeline, so both are driven together to
> maximize the odds of a visible result regardless of which guess is right.
>
> Re-registered every `on_hozelda_execute_pre()` tick (cheap to re-set; avoids tracking
> creation/respawn state separately) rather than once at spawn, since the mod never observes
> `daHoZelda_c::create()` run.
>
> Rotation axis: build #384's dual-joint fix did make `CHIN` visibly move, but only as a
> side-to-side sweep with the mouth itself staying shut -- i.e. the joint choice was right but
> the rotation axis wasn't. `daHoZelda_c::modelCallBack()`'s own neck/body code
> (`d_a_hozelda.cpp`) settles this precisely for this exact skeleton:
> `mDoMtx_stack_c::XrotM(mNeckAngle.y)` drives the yaw/turn component (`.y`) and
> `mDoMtx_stack_c::ZrotM(-mNeckAngle.x)` drives the pitch/nod component (`.x`). We were using
> `XrotM` -- the yaw axis, matching the reported side-to-side sweep exactly -- so switched to
> `ZrotM`, the pitch axis real nodding uses, which should actually hinge the jaw open/closed
> instead of sweeping it sideways.

## `find_zelda_npc_proxy()` (forward declaration)

> Forward-declared: defined further down alongside the rest of the NPC-talk-proxy machinery, but
> needed here so the companion's own execute() post-hook can re-sync the proxy immediately (see
> on_hozelda_execute_post()'s comment below for why).

## `on_hozelda_execute_post()` — proxy re-sync call

> Re-sync the invisible talk proxy's position/attention data right now, as early as possible
> after the companion's own position is finalized for this tick, instead of waiting for the
> proxy's own Execute() (whenever in the per-frame actor-execution order the engine happens
> to run it) to pick it up. Previously, whenever the engine ran the proxy's Execute() before
> the companion's for a given tick, the proxy was left reading the companion's PREVIOUS-tick
> position -- a one-frame-stale target. While Epona stood still that lag was invisible (the
> position doesn't change tick to tick), but while she was moving (player-ridden, or
> autonomously following while dismounted -- see PR #31), it created a persistent, visible
> gap between the proxy's logical (lock-on) position and the companion's actual rendered
> position, which was then large enough to throw off check_flontofplayer()'s facing-angle
> gate at normal conversation range, making her impossible to L-target while moving at all.

## `set_anm_auto_target()` — `ANM_HS_RUN_DASH` (`0x11`) branch

> ANM_HS_RUN_DASH: Epona's "Lash" speed-burst anim, most noticeable right after a
> skidding stop when immediately accelerating again. Vanilla keeps this distinct
> from the calmer run anims below (mapped to 0xF, not 0xE); merging it into 0xE
> here made Zelda use a body pose that doesn't account for the dash's extra forward
> lurch, leaning her head into Link's back.

## `zelda_talk_event_active()`

> Whether a scripted event is currently running with our invisible talk proxy as its partner --
> true as soon as the player presses the talk button on lock-on, well before any text is
> actually on screen. This is deliberately broader than "dialogue text is visible" (requiring
> dMsgObject_isTalkNowCheck() too): real NPCs (daNpc_Zelda_c::talk(), d_a_npc_zelda.cpp) start
> turning to face the player during this same window, before their first line of dialogue
> appears, and we want Epona's facing correction below to begin at the same moment.

## `kZeldaTalkHeadTrackCone`

> daHoZelda_c's own faux head-tracking (set_neck_angle_auto_target(), above) only turns her neck
> to look at the player when they fall within roughly a 90-degree cone of her current facing
> (zelda->shape_angle.y, which mirrors Epona's own via setMatrix()); talked to from outside that
> cone (e.g. approached/locked-on from behind), her neck can't turn far enough and she can't
> visually look at the player for the whole conversation, unlike a normal standalone NPC who'd
> simply turn their body to face whoever they're talking to. Zelda has no body of her own to
> turn -- she's rigidly attached to Epona's root matrix -- so rotate Epona instead, but only by
> the minimum amount needed to bring the player within a tighter 50-degree cone of her facing,
> not all the way around to directly facing them -- a comfortable margin inside the native
> 90-degree engagement threshold rather than sitting right at its edge.

## Dismounted NPC-style interaction with the spawned Zelda — architecture overview

Above the declaration of `kZeldaTalkMessageGroup` and the rest of the Zelda-talk feature below:

> --- Dismounted NPC-style interaction with the spawned Zelda ---
>
> Z-targeting Zelda while dismounted and pressing the action button opens a short, original
> message box, instead of the normal proximity "talk" prompt icon that every other NPC uses --
> that icon would visually and functionally collide with Epona's own "mount" prompt, since Zelda
> is always within mounting distance of the horse she rides on. Always enabled, no toggle.
>
> Rather than hand-rolling a conversation (manually ticking a dMsgFlow_c, forcing
> dComIfGp_event_runCheck(), neutralizing raw pad state, etc. -- all of which turned out to be an
> unreliable, ever-growing pile of special cases), this spawns a second, completely invisible
> daNpc_Zelda_c actor (a real, genuinely talk-capable vanilla NPC class) co-located with the
> visible companion every tick, and lets the *real* NPC-talk pipeline drive everything: Z-locking
> it (gated to only be possible while dismounted and otherwise eligible, see
> on_npc_zelda_execute_post() below) lets daAlink_c::orderTalk() call the real
> fopAcM_orderTalkEvent() on it when the action button is pressed, exactly like any other NPC;
> from there, the proxy's own inherited daNpcT_c::evtProc()/evtTalk()/talk() state machine
> autonomously drives its mFlow (redirected to our custom BMG content via the
> dMsgFlow_c::getInitNodeIndex() hook below) every tick via its own Execute() call, with zero
> further mod intervention needed -- including camera framing, pause-menu gating, reticle
> hiding, and freezing the player's movement/combat input, all for free, since the engine can't
> tell this apart from a real NPC conversation.
>
> The proxy is kept purely invisible/non-physical (Draw()/setCollision() hooked to skip their
> real work entirely) and is positionally glued to the companion every tick, so from the
> player's perspective they appear to be locking onto and talking to the companion herself.

## `kZeldaTalkMessageGroup`

> BMG message group our custom greeting is registered under. Group 0 is the always-resident
> "common" message archive (the same one Midna's menu uses), so it resolves correctly no matter
> which field/stage Zelda is approached in.

## `kZeldaTalkFlowId`

> Synthetic flow id encoded into the proxy's spawn angle.x (see daNpc_Zelda_c::getFlowNodeNo(),
> which reads it back as mFlowNodeNo) and later passed straight through to
> dMsgFlow_c::init()/getInitNodeIndex() by her own native talk() state. It intentionally falls in
> dMsgObject_changeFlowGroup()'s "common group" range ([3000, 0x8000)) so the engine's own
> group-resolution logic binds everything to kZeldaTalkMessageGroup correctly on its own, and
> intentionally doesn't match any real vanilla flow label, which is exactly what makes
> getInitNodeIndex() hookable: its own lookup for this id always misses, giving us a clean,
> unambiguous point to redirect to our actual registered starting node instead.

## `s_zeldaTalkWolfNodeId`

> Separate starting node for the wolf-form conversation (same graph/group, see
> register_zelda_talk_flow()); on_msg_flow_get_init_node_index_pre() picks between this and
> s_zeldaTalkNodeId based on the player's form at the moment the conversation actually starts.

## `s_zeldaTalkPage1MessageId`

> Page 1's registered id, kept around so update_zelda_talk_heal_effect() (further down) can tell
> -- by comparing it against the live msg_class's msg_idx -- whether the conversation is
> currently showing page 1 specifically, as opposed to page 2 or no conversation at all.

## `kZeldaTalkWolfPauseFrames`

> One second, in the engine's actual 30Hz tick domain -- used by the wolf-form conversation both
> for the mid-sentence pause between "Midna." and "Do you really..." and for the pause before
> "So undignified..." reveals on page 2.

## `kZeldaTalkHealEffectDelayFrames`

> How long (in the engine's actual 30Hz tick domain) page 1 holds the player's continue input
> once typing finishes, via the input_after_delay() tag registered on it below -- long enough for
> the heal particle trail (and its accompanying Z2SE_FAIRY_S_LV/Z2SE_FAIRY_S_FLY_RTT sounds) to
> actually be heard/seen before the conversation can move on, without making the player wait too
> long in dialogue. ~3 seconds.

## `kZeldaTalkHealEffectDurationFrames`

> How long the heal particle trail itself keeps emitting before we stop it (which also stops its
> accompanying Z2SE_FAIRY_S_LV loop via stop_zelda_talk_heal_effect()'s seStop() call, cutting
> both off together). Shorter than daObjYOUSEI_c::MoveAction()'s own ~120-tick (4 second) native
> spiral so the player isn't held in dialogue too long; matches the input delay above so the
> spiral finishes closing in right as the player becomes able to advance, rather than getting cut
> off mid-spin or lingering after input unlocks.

## `kZeldaTalkHealAmount`

> 8 hearts, in the quarter-heart units dComIfGp_setItemLifeCount() expects -- the same amount
> item_func_RECOVER_FAILY() (d_item.cpp) grants for drinking a bottled fairy natively.

## `kZeldaTalkHealEffectStartHeight`

> Height (relative to the player's own base position) the spiral starts at -- matches
> daObjYOUSEI_c::MoveAction()'s own mSpeedVTarget, which begins at 0.0f (level with the player's
> feet) the instant the fairy leaves the bottle, then climbs every tick thereafter.

## `kZeldaTalkHealEffectEndHeightAboveHead`

> Extra clearance above the player's own head-top position (daPy_py_c::getHeadTopPos(), already
> accounting for standing/crouching/swimming/wolf-form height differences) the spiral's final
> height targets, so the effect reliably finishes just above the player's head rather than at a
> fixed, possibly mismatched height -- matching MoveAction()'s own ever-rising mSpeedVTarget,
> which keeps climbing (not holding a constant height) for the entire spiral.

## `kZeldaTalkHealEffectStartRadius`

> Orbit radius (in game units) at the start and end of the spiral -- shrinks from wide to tight
> over the effect's duration, same as daObjYOUSEI_c::MoveAction()'s own radius term
> (10.0f + 130.0f * (field_0x5d6 / 120.0f)), which spirals the fairy in toward the player rather
> than holding a fixed distance.

## `kZeldaTalkHealEffectAngularVelocity`

> Angular velocity, in BinAngle units per tick -- the same magnitude MoveAction() uses
> (field_0x5dc = -0x1CB8) to spin the fairy around the player as it spirals in. Negated sign
> doesn't matter cosmetically; what matters is that it's fast enough to read as "circling"
> rather than "drifting".

## `kZeldaTalkHealEffectSpinRampFrames`

> How many frames the spin speed takes to ease in from a standstill, mirroring MoveAction()'s own
> var_f31 ramp (which reaches 1.0 after (25 frames)/field_0x5d6 of ramp-up).

## `s_zeldaTalkHealGlow0EmitterId` (and the sibling GLOW01/STAR03 emitter handles)

> Handles for the three particle emitters below, tracked the same way daAlink_c's own effect
> fields are (e.g. field_0x31c4 in d_a_alink.h): 0 means "no emitter yet / already stopped",
> otherwise dComIfGp_particle_getEmitter() resolves it back to a live JPABaseEmitter. These three
> specific IDs (GLOW00/GLOW01/STAR03) are the ones daObjYOUSEI_c::MoveAction() spawns while
> actually flying/spiraling toward the player -- distinct from BinAction()'s GLOW00/STAR02 pair,
> which is the fairy's much plainer idle "sitting still in the bottle" glow (the wrong, static
> effect this code originally copied).

## `s_zeldaTalkHealEffectFramesLeft`

> Countdown until stop_zelda_talk_heal_effect() is called; -1 while the effect isn't playing.

## `s_zeldaTalkHealEffectAngle`

> Current orbit angle (BinAngle units), advanced every tick by refresh_zelda_talk_heal_effect().

## `refresh_zelda_talk_heal_effect()`

> Re-issues all three particle_set() calls against their existing handles, anchored to a point
> that spirals in and climbs around the player -- matching daObjYOUSEI_c::MoveAction()'s own
> orbit-and-close motion (an XZ offset, rotated by an ever-advancing yaw, whose radius shrinks
> over the effect's duration while its height climbs from the player's feet toward just above
> their head), rather than a single static point. Also re-issues the fairy's own ambient flight
> sound (Z2SE_FAIRY_S_LV, the same "level" sound Execute() loops every tick while she's airborne)
> positioned at that same spiraling point. This MUST be called every tick the effect is meant to
> keep playing: dPa_control_c::level_c::execute() (d_particle.cpp) stops and tears down ("cuts")
> any "level" emitter -- and Z2SeMgr's own seStartLevel()/moveLife()-style "level" sounds work the
> same way -- whose handle/soundID isn't re-submitted that same tick. Every native example of
> this pattern (e.g. daAlink_c::setBottleEffect(), called once per execute() tick) re-calls
> setEmitter()/particle_set() every frame for exactly this reason.

## `trigger_zelda_talk_heal_effect()`

> Restores 8 hearts (dComIfGp_setItemLifeCount(), the same call item_func_RECOVER_FAILY() makes
> for a native bottled fairy), plays the fairy's own one-shot "release" whoosh
> (Z2SE_FAIRY_S_FLY_RTT, the same sound daObjYOUSEI_c::CareAction() plays the instant she starts
> flying toward the player), and spawns the glow/star particles she wears on her own body while
> actually flying/healing (MoveAction()'s ID_ZF_J_FAIRY00_GLOW / ID_ZF_J_FAIRY01_GLOW /
> ID_ZF_J_FAIRY03_STAR emitters), spiraling in around the player instead of an actual fairy actor,
> since only the cosmetic sparkle trail (and its sound) was wanted, not the fairy.

## `kZeldaTalkSpeakerId`

> Z2SpeechMgr2's voice-bank "speaker" id, which gates TWO separate native sound systems:
>   - jmessage_tSequenceProcessor::do_character()'s one-shot messageSePlay() -> playOneShotVoice()
>     (a per-message "mood grunt", gated only on speaker_mood being set -- we don't use this).
>   - jmessage_tMeasureProcessor::do_end()'s Z2GetSpeechMgr2()->setString(..., speaker, mood),
>     which is the REAL per-character typing click: Z2SpeechMgr2::setString() maps speaker ids
>     0-3 to mVoice=99 and returns immediately (permanently disabling the per-character sound for
>     the rest of that message -- the bug an earlier id of 1 accidentally hit), while any id
>     outside its big per-NPC-archetype table (e.g. Midna's 0x13/0x15, Goron's 0xe, etc.) falls
>     through to its `default: mVoice = 11` bucket, which setTextCount() (called every tick as
>     more characters become visible) uses to play Z2SE_SY_TEXT_OUT -- the same generic
>     per-character click ordinary human NPCs use. 5 is simply an id that doesn't appear anywhere
>     in either native table, landing cleanly in that default/generic bucket.

## `spawn_zelda_npc_proxy()` — `params.angle` flow-id encoding

> angle.x doubles as the proxy's own flow-id field (see daNpc_Zelda_c::getFlowNodeNo()),
> read once at create() time -- encode our synthetic flow id here instead of a real facing
> pitch, matching the Dawnlight mod's own hub-NPC spawn convention.

## `register_zelda_talk_flow()` — `register_page` lambda

> Registers a single message page, for every supported language, and returns its message id.
> i_configure fills in the page's content/style on a fresh MessageBuilder (e.g. via .text()
> and/or tag helpers like .player_name()); it's invoked once per language, but tag bytes
> (MSGTAG_PLAYER_NAME, MSGTAG_HORSE_NAME, ...) are resolved by the engine at display time
> from the current save data, not baked in per-language here. The underlying
> RegisteredMessage handles are kept alive for the mod's lifetime in s_zeldaTalkMessages
> (register_message()'s returned handle owns the BMG entry).

## `register_zelda_talk_flow()` — `page1Id` (two-page greeting)

> Two-page greeting. <player_name>/<horse_name> are embedded as raw BMG tags
> (MSGTAG_PLAYER_NAME via .player_name(), MSGTAG_HORSE_NAME via raw_tag()) rather than text
> fetched from dComIfGs_getPlayerName()/dComIfGs_getHorseName() up front, so the name shown
> always reflects whichever save is actually loaded when the player talks to her, not
> whichever save (if any) happened to be loaded back when the mod registered this message.
> Each page is its own node so the player pages through it with the action button exactly
> like any other NPC conversation, and the last page's node points at the special
> mods::flow::kEnd sentinel, which naturally closes the message window and hands control
> back to normal gameplay once the player dismisses it -- no extra closing logic needed.

## `register_zelda_talk_flow()` — `input_after_delay()` call

> input_after_delay() emits MSGTAG_BOXATLEAST (group 0, type 54): the engine keeps page 1's
> box up and ignores the player's continue input until kZeldaTalkHealEffectDelayFrames have
> elapsed, giving update_zelda_talk_heal_effect()'s particle trail (triggered the instant
> typing finishes, see below) a moment to actually play before the player can page past it.

## `register_zelda_talk_flow()` — `wolfPage1Id` (wolf-form conversation)

> Separate two-page conversation for when the player approaches her in wolf form, addressed
> to Midna rather than the player directly. Page 1 uses pause() (MSGTAG_PAUSE, group 0 type
> 7) to insert a one-second beat between "Midna." and "Do you really..." without interrupting
> the normal per-character typing/sound on either side of it. Page 2 pairs that same pause
> with a raw MSGTAG_INSTANT (group 0 type 1) tag immediately after it: do_isReady()'s typing
> pacer processes the stream one token at a time regardless of whether it's a tag or a
> character, so the pause fully elapses first (nothing shown), and only once it does does the
> instant tag get processed -- at which point it sets the same "reveal everything right now"
> batch flag the native press-A/hold-B instant-text shortcuts use, so the rest of the page's
> text (just "So undignified...") pops in all at once rather than typing out letter by
> letter.

## `on_npc_zelda_draw_pre()`

> daNpc_Zelda_c::Draw() renders her actual geometry; skip it unconditionally for our proxy so
> she's never visible, without touching mHide/mNoDraw (daNpc_Zelda_c::beforeMove() zeroes
> attention_info.flags whenever either of those is set, which would silently undo the
> targetability gating in on_npc_zelda_execute_post() below).

## `on_npc_zelda_set_collision_pre()`

> daNpc_Zelda_c::setCollision() registers a real world collision cylinder every tick (unless
> mHide is set, which we can't use -- see the Draw() hook above); skip it entirely for our proxy
> so she can't physically push Link or Epona around while invisibly co-located with the
> companion.

## `zelda_talk_may_be_targeted()`

> Whether the proxy is currently allowed to be targeted/talked to at all: the companion must
> exist, the player must be dismounted, Zelda mustn't be hidden for a cutscene, and no scripted
> event (including her own conversation, once started) may already be running.

## `sync_zelda_npc_proxy_from_companion()`

> Keeps the invisible proxy glued to the companion every tick, and keeps its native lock-on
> "talk" eligibility in sync with zelda_talk_may_be_targeted(), so the engine's own "Speak"
> action-button prompt only ever appears while she's actually allowed to be talked to.
>
> daNpc_Zelda_c::setParam() (called earlier this same tick, from execute()) unconditionally
> turns both fopAc_AttnFlag_SPEAK_e (the proximity "talk" icon -- which would visually collide
> with Epona's own "mount" prompt) and fopAc_AttnFlag_TALK_e (the lock-on-only eligibility we
> actually want) back on; this runs synchronously right after that within the same Execute()
> call, so nothing outside it (in particular dAttention_c's own lock-on candidate search) ever
> observes the transient over-permissive state setParam() leaves behind.
>
> Glues the invisible proxy's position/angle/room/attention-point to the companion's own,
> current-tick values. Factored out so it can be called from TWO places: the companion's own
> execute() post-hook (as soon as the companion's position is final for this tick, while Epona
> may still be mid-stride that same frame) and the proxy's own execute() post-hook (a safety-net
> re-sync, in case the companion's hook ran before the proxy existed yet this tick). Calling it
> twice in one tick is harmless -- it's a pure copy, not an accumulation -- and ensures whichever
> of the two actors' Execute() the engine happens to run LAST this tick, the proxy never ends up
> a whole frame stale relative to the companion by the time dAttention_c's lock-on candidate
> scan runs. A single frame of staleness while Epona is moving (autonomously following, or being
> ridden) was previously enough positional lag between the logical (proxy) and visual (companion
> model) target points to throw off check_flontofplayer()'s facing-angle gate at close range,
> even though the lag was imperceptible (and inconsequential) while she stood still.

## `sync_zelda_npc_proxy_from_companion()` — `proxy->mCurAngle.y` sync

> daNpc_Zelda_c::setAttnPos() (called earlier this same tick, inside execute()) derives
> attention_info.position from mCurAngle.y, not shape_angle.y -- it's only ever set once
> at create() time from the spawn angle otherwise, so keep it in sync too for the sake of
> setAttnPos()'s other (cosmetic, head/eye-tracking) uses of it.

## `sync_zelda_npc_proxy_from_companion()` — `attention_info.position` override

> Override the lock-on target point directly instead of trusting setAttnPos()'s own
> computation for it (that native math rotates a small local bust offset through
> mCurAngle.y via a from-scratch matrix, an extra unverified source of uncertainty for a
> value this entire feature's usability hinges on), or re-deriving our own Y offset from
> scratch. The real, visible daHoZelda_c companion already recomputes its own
> attention_info.position natively every tick in setBowModel()
> (current.pos.y + 80.0f -- see d_a_hozelda.cpp), and the pre-proxy implementation of
> this feature targeted that real actor directly and was reliably lock-on-able. Reuse
> that exact value instead of our own +170.0f guess (which put the proxy's attention
> point ~90 units higher than the companion's own, pushing the real-world vertical gap
> to a dismounted player's own attention point past most dist_table windows).

## `sync_zelda_npc_proxy_from_companion()` — `attention_info.distances[fopAc_attn_TALK_e]` override

> The proxy's class (daNpc_Zelda_c) defaults attention_info.distances[TALK_e] to a
> dist_table entry (index 219: mDistMax=300, mUpperY=300, mLowerY=-300) sized for a
> normal ground-level NPC. Override to dist_table[18] (mDistMax=500, mUpperY=800,
> mLowerY=-800, mAngleSelect=0x0, i.e. no facing-direction restriction at all), a more
> generous entry, for extra margin now that the Y-offset bug above is also fixed.

## `on_npc_zelda_execute_post()` — `dEvtCnd_CANTALK_e` latch

> dAttention_c::calcWeight() ANDs attention_info.flags together with an additional,
> separate gate: check_event_condition(TALK_e, eventInfo.getCondition()), which requires
> dEvtCnd_CANTALK_e to already be latched onto the actor's eventInfo. The *only* place that
> ever sets it (daNpcT_c::evtOrder(), base d_a_npc.cpp) only fires once per tick, strictly
> *before* this post-hook runs (it's called from inside daNpc_Zelda_c::Execute() itself, via
> the base daNpcT_c::execute()), and only when it sees attention_info.flags already carrying
> SPEAK_e/TALK_e -- i.e. the flags *we're setting right here, this same tick, after it
> already ran*. And since f_op_actor.cpp's per-actor beforeProc() unconditionally resets
> eventInfo's condition bits to just dEvtCnd_CANDEMO_e at the very start of every tick
> (before Execute() runs), CANTALK_e isn't sticky across ticks either: it must be
> re-latched by something, every single tick, while eligible. Whether evtOrder() itself
> actually runs that tick further depends on native per-actor order-wait/event-exec gating
> we don't control. Rather than chase that race, latch the bit ourselves, directly and
> unconditionally, right after we decide our own eligibility -- removing any dependency on
> evtOrder()'s internal timing/gating ever lining up with our flag toggle again.

## `on_msg_flow_get_init_node_index_pre()`

> dMsgFlow_c::init(partner, i_flowID, ...) (called by the proxy's own native talk() state via
> initTalk(mFlowNodeNo, ...), with mFlowNodeNo read once at create() time from our synthetic
> kZeldaTalkFlowId spawn angle, see spawn_zelda_npc_proxy()) ends with
> setNodeIndex(getInitNodeIndex(mFlow), ...). Since kZeldaTalkFlowId intentionally doesn't match
> any real vanilla flow label, the real getInitNodeIndex() lookup for it always misses; intercept
> it here and hand back our actual registered starting node instead, exactly once per
> conversation.

## `on_msg_flow_get_init_node_index_pre()` — wolf-form node pick

> Pick the wolf-form conversation whenever the player is the one currently talking to
> her while transformed (daPy_py_c::checkWolf(), same FLG1_IS_WOLF the game itself uses
> to tell Link and Wolf Link apart) -- checked once, right as the conversation starts,
> so which node we hand back stays fixed for its whole duration even if the player
> somehow transforms mid-conversation.

## `sync_zelda_npc_proxy_lifecycle()`

> Spawns/despawns the invisible talk proxy alongside the companion's own lifecycle: she only
> needs to exist while the companion does, and self-heals (via find_zelda_npc_proxy()'s own
> liveness check) if the engine ever deletes her out from under us.

## `zelda_talk_is_active()` / `update_zelda_mouth_animation()`

> Whether the conversation currently on screen is specifically the one we drive through the
> proxy (as opposed to some other NPC's, or none at all) -- used to gate the companion's faux
> lip-sync. Drives `s_zeldaMouthAngle` (applied every frame by `hozelda_mouth_joint_callback()`,
> see its comment above) toward an "open" pose while our dialogue is the one actually on screen
> and `dMsgObject_isMouthCheck()` -- the same engine-wide text-reveal-rhythm flag real talking
> NPCs read in `ctrlMsgAnm()` -- says this instant is a "mouth open" beat, easing back to closed
> the rest of the time (including whenever she isn't actively talking to us at all).
>
> `kZeldaMouthOpenAngle` was doubled from its original `0x500` (~7 degrees) to `0xA00`
> (~14 degrees) after build #384 testing showed no observable movement at the smaller angle,
> in case subtlety (rather than joint choice) was the whole problem.
>
> `dMsgObject_isMouthCheck()` isn't the rapid per-character toggle its name suggests -- native
> `jmessage_tSequenceProcessor` (`d_msg_class.cpp`) only clears `mMouthCheck` back to 0 around
> punctuation-driven pauses (`!`, full-width punctuation, page breaks), so for ordinary prose
> it reads as "true" continuously for the whole run of characters between pauses. Real NPCs get
> away with reading it directly because `ctrlMsgAnm()` only uses it to gate playback of an
> already-authored, already-animated talk-loop BCK (`ANM_FLAG_4000`) -- the visual "flapping" is
> baked into that loop, not into the boolean. Zelda has no such authored loop to gate, so easing
> straight toward `kZeldaMouthOpenAngle` on a bare `isMouthCheck()` read held her jaw open and
> static for the whole sentence instead of chattering (build #384 feedback). `s_zeldaMouthFlapTimer`
> / `s_zeldaMouthFlapOpen` resynthesize the missing flap ourselves: while a mouth-open beat is
> underway, flip the target between fully open and fully closed every `2 + cM_rndF(4)` frames
> (randomized so it doesn't read as a metronome), snapping back to closed the instant the beat
> ends.
>
> The jaw was also rotating the wrong way (upward instead of down, per build #384 testing) with
> a bare `ZrotM(s_zeldaMouthAngle)`; mirrored the sign `daHoZelda_c::modelCallBack()` itself uses
> for its own `ZrotM(-mNeckAngle.x)` pitch rotation and negated it to `ZrotM(-s_zeldaMouthAngle)`.

## `update_zelda_talk_heal_effect()`

> Ticks the countdown started by trigger_zelda_talk_heal_effect(), and fires that effect itself
> exactly once per page-1 display, the instant its text finishes typing out: msg_class::mode
> transitions away from fopMsg_MODE_MSG_TYPING_e (0x06, "actively revealing characters") the
> same tick the last character appears -- the same mode value daNpcT_c::ctrlMsgAnm() reads for
> real NPCs' lip-sync -- so watching for that falling edge, rather than guessing a frame count,
> catches "done typing" regardless of text length or the player's text-speed setting. Compared
> against s_zeldaTalkPage1MessageId (rather than just zelda_talk_event_active()) so page 2 --
> which shares the same event/flow -- is never mistaken for page 1.

## `mod_update()` — keep Zelda visible during her own dialogue

Above `if (zelda_talk_event_active()) { s_hideSpawnedZeldaInCutscene = false; }`:

> Her own talk event runs through the same dComIfGp_event_runCheck()/isOrderOK() machinery
> native scripted cutscenes do (see zelda_talk_event_active()'s own dComIfGp_event_runCheck()
> check above), which is why checkHorseDemoMode() -- and therefore inScriptedCutscene above --
> also reports true for the entire duration of her dialogue: Epona isn't registered as this
> synthetic event's "staff", so daHorse_c::setDemoData() treats it exactly like being sidelined
> for an unrelated cutscene. Unconditionally keep her visible while she's actually the one
> talking, regardless of the "Show Zelda During Cutscenes" setting -- that setting was never
> meant to apply to her own dialogue, and hiding her mid-conversation would be jarring.
