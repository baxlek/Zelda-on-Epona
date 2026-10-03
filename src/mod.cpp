#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

// Game includes
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_b_gnd.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_lib.h"
#include "Z2AudioLib/Z2SeMgr.h"

DEFINE_MOD();

IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(LogService, svc_log);

// Whether this mod's own spawned Zelda should stay visible on Epona during scripted story
// cutscenes. Off by default: by default she's hidden for the duration of any such cutscene (and
// reappears once it ends) rather than potentially clashing with the scene's own staging (wrong
// orientation/animation, overlapping props, etc.) the way earlier attempts at patching her up for
// every individual cutscene kept doing. Exposed as a single toggle in the mod's panel in the host
// Mods window.
static ConfigVarHandle g_cvarShowInCutscenes = 0;

static bool show_zelda_in_cutscenes() {
    bool value = false;
    if (g_cvarShowInCutscenes == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarShowInCutscenes, &value) != MOD_OK) {
        return false;
    }
    return value;
}

// Whether this mod's own spawned Zelda should auto-target and fire her Light Arrows at nearby
// enemies, the same way she already does at Ganondorf during the real horseback duel, instead of
// just riding passively. On by default. Exposed as a toggle in the mod's panel in the host Mods
// window for anyone who'd rather she stay a passive passenger.
static ConfigVarHandle g_cvarAutoTargetEnemies = 0;

static bool auto_target_enemies_enabled() {
    bool value = true;
    if (g_cvarAutoTargetEnemies == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarAutoTargetEnemies, &value) != MOD_OK) {
        return true;
    }
    return value;
}

// The stage name used by the game to load the horseback-Zelda actor (see `d_stage.cpp`'s
// OBJNAME table: OBJNAME("HoZelda", fpcNm_HOZELDA_e, -1)).
static const char* kHoZeldaStageName = "HoZelda";

// The ActorId of the horseback-Zelda actor this mod spawned, if any. We only ever manage an
// actor we spawned ourselves; a HoZelda actor placed by the vanilla game (e.g. during the
// horseback archery duel against Ganondorf, where Zelda rides alone) is always left untouched.
static ActorId s_spawnedZeldaId = 0;
static bool s_hasSpawnedZelda = false;

static fopAc_ac_c* find_spawned_zelda() {
    if (!s_hasSpawnedZelda) {
        return nullptr;
    }

    // Actor creation (`fopAcM_create`/`fpcSCtRq_Request`) is a multi-phase request that can take
    // more than one frame to complete (e.g. resource loading), during which the new actor sits in
    // the engine's "create queue" rather than its normal execute queue. While that's happening,
    // `fopAcM_SearchByID` reports "not found" (a null actor, but still a success return) even
    // though the actor we spawned is still on its way and hasn't actually disappeared. Treating
    // that as "the actor is gone" made us give up tracking it and spawn a second HoZelda while the
    // first spawn request was still in flight, producing two overlapping HoZelda actors (the
    // "double Zelda" bug). `fpcM_IsCreating` lets us tell the two cases apart.
    if (fpcM_IsCreating((fpc_ProcID)s_spawnedZeldaId)) {
        return nullptr;
    }

    fopAc_ac_c* actor = nullptr;
    fopAcM_SearchByID((fpc_ProcID)s_spawnedZeldaId, &actor);
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_HOZELDA_e) {
        // The actor we spawned no longer exists (e.g. deleted on a room change), or its ProcID
        // has since been recycled by the engine for an unrelated actor (ProcIDs are reused once
        // freed). Either way, we no longer have a spawned Zelda to track.
        s_hasSpawnedZelda = false;
        return nullptr;
    }
    return actor;
}

// Whether `actor` is the HoZelda actor this mod spawned itself (as opposed to a story-placed one,
// e.g. the real Ganondorf duel's own Zelda), used throughout to scope hooks to our own actor only.
static bool is_spawned_zelda(const fopAc_ac_c* actor) {
    return s_hasSpawnedZelda && actor != nullptr &&
           (ActorId)fopAcM_GetID(actor) == s_spawnedZeldaId;
}

static void spawn_zelda_on_horse(daHorse_c* horse) {
    ActorSpawnParams params{};
    params.parameters = 0;
    params.argument = 0;
    params.room_num = fopAcM_GetRoomNo(horse);
    params.position = {horse->current.pos.x, horse->current.pos.y, horse->current.pos.z};
    params.angle = {horse->shape_angle.x, horse->shape_angle.y, horse->shape_angle.z};
    params.scale = {1.0f, 1.0f, 1.0f};

    ActorId newId = 0;
    ModResult result = svc_actor->create_actor_from_name(mod_ctx, kHoZeldaStageName, &params, &newId);
    if (result != MOD_OK) {
        mods::log::warn("failed to spawn HoZelda actor: {}", (int)result);
        return;
    }

    s_spawnedZeldaId = newId;
    s_hasSpawnedZelda = true;
}

static void remove_spawned_zelda() {
    if (!s_hasSpawnedZelda) {
        return;
    }

    svc_actor->delete_actor(mod_ctx, s_spawnedZeldaId);
    s_hasSpawnedZelda = false;
}

// `daHorse_c::callHorseSubstance()` (the grass-whistle horse call) special-cases *any* moment
// where the current HoZelda passenger is riding alone (`checkSingleRide()`, true whenever Link
// isn't mounted) by assuming the scripted final-duel reunion is underway: instead of the normal
// gallop-to-player behavior, it silently plays Epona's neigh, jumps straight to the duel's
// "arrival" demo state, and never actually moves Epona. Since this mod keeps Zelda riding solo
// for the entire game (not just the real duel), that special case would otherwise fire every
// time the whistle is used while she's mounted.
//
// A real scripted duel always uses its own, story-placed HoZelda actor, never one this mod
// spawned. So immediately before the original runs, if the actor currently attached to the horse
// is ours, we briefly detach it (`setZeldaActor(nullptr)`) so `callHorseSubstance` takes its
// normal path instead. This has no lasting effect: `daHoZelda_c::execute()` unconditionally
// reattaches our Zelda to the horse (`horse->setZeldaActor(this)`) on every single tick anyway,
// so by the very next frame she's back exactly where she was.
DEFINE_HOOK(&daHorse_c::callHorseSubstance, HorseCallSubstance);

static HookAction on_horse_call_substance_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    fopAc_ac_c* currentZelda = horse->getZeldaActor();
    if (s_hasSpawnedZelda && currentZelda != nullptr &&
        (ActorId)fopAcM_GetID(currentZelda) == s_spawnedZeldaId) {
        horse->setZeldaActor(nullptr);
    }
    return HOOK_CONTINUE;
}

// `daHoZelda_c::setMatrix()`'s own, unmodified computation for the solo (front saddle) seat --
// `current.pos` from a fixed local offset through `horse->getRootMtx()`, `shape_angle` copied
// straight from `horse->shape_angle` -- is value-for-value the same technique
// `daAlink_c::setSyncHorsePos()` uses for Link's own riding position outside of a few specific
// movement animations (`mDoMtx_multVec(horse_p->getRootMtx(), &l_localHorseRidePos,
// &current.pos); shape_angle = horse_p->shape_angle;`), just with a different local offset
// constant. Since Link, using that exact computation, never clips into or floats off of Epona
// while she's startled and rears up, the computation itself isn't the problem; every attempt at
// improving on it instead (deriving rotation from the live saddle joint; re-projecting Zelda's
// local offset through that joint; nudging only its vertical component) ended up regressing one
// axis while fixing another (leaning too far back; clipping into Epona's risen body; floating
// above the saddle), because Zelda's own local offset sits much further forward and higher than
// Link's -- any deviation from the plain root-matrix computation gets amplified by that larger
// lever arm in a way it never does for Link.
//
// So rather than attempting to derive a better position/rotation for the rearing case, this just
// borrows Link's own riding position outright for it: while Epona is mid-rear
// (`horse->checkTurnStand()`), Zelda's seat is placed using Link's own local ride offset
// (`daAlink_c`'s `l_localHorseRidePos`, duplicated below since the original is a file-local
// constant in `d_a_alink.cpp`) through the exact same root-matrix-plus-direct-shape_angle-copy
// computation already proven not to clip or float for Link in that same state. Outside of a rear,
// this hook does nothing at all: vanilla's own computation (with Zelda's own local offset) was
// never reported to look wrong there, so it's left completely untouched.
//
// `checkTurnStand()` only reports true for the held-rear portion in the *middle* of Epona's whole
// startle/turn animation (see `daHorse_c::procTurn()`'s `field_0x1780`/`field_0x1774` frame-window
// check), not its full duration. Swapping the local offset outright the instant that window opens
// (and again the instant it closes) was an abrupt, one-frame jump between Zelda's own offset and
// Link's -- small, but visible as a brief backward slide right as the rear kicks in. To smooth that
// over, the two offsets are now blended across a few frames (via `cLib_chaseF`) instead of swapped
// in one step, so Zelda's seat eases from one to the other alongside the rear starting/ending rather
// than snapping.
//
// Scoped to our own spawned Zelda only: the vanilla duel's story-placed HoZelda already works
// correctly as-is, so it's left untouched to avoid any risk of regressing it. Scoped to the solo
// seat only (no Link riding): the dual-ride (rear) seat already works correctly via vanilla's own
// computation and was never part of this bug.
DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);

static void on_hozelda_set_matrix_post(ModContext*, void* args, void*, void*) {
    // How much of a rear's local-offset blend (0 = fully vanilla, 1 = fully Link's offset) has
    // eased in so far; chased every frame toward 0 or 1 depending on `checkTurnStand()` so the
    // transition is smooth in both directions instead of an instant swap.
    static f32 s_turnStandBlend = 0.0f;

    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!s_hasSpawnedZelda || (ActorId)fopAcM_GetID(zelda) != s_spawnedZeldaId) {
        s_turnStandBlend = 0.0f;
        return;
    }

    // Dual-ride (Link also on Epona) already works correctly via vanilla's own computation; only
    // the solo seat (no Link riding) is in scope for this fix.
    if (daPy_getLinkPlayerActorClass()->checkHorseRide()) {
        s_turnStandBlend = 0.0f;
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || zelda->model == nullptr) {
        s_turnStandBlend = 0.0f;
        return;
    }

    // Eases toward 1 while rearing, back toward 0 once the rear ends; 1/8th per frame fully blends
    // in about 8 frames (roughly a tenth of a second at 60 FPS), fast enough to not be noticeable as
    // its own separate motion, but slow enough to erase the one-frame snap.
    cLib_chaseF(&s_turnStandBlend, horse->checkTurnStand() ? 1.0f : 0.0f, 1.0f / 8.0f);

    // Fully settled outside of a rear: vanilla's own computation (already run by the time this
    // post-hook fires) is left completely untouched.
    if (s_turnStandBlend <= 0.0f) {
        return;
    }

    // Vanilla's own solo-seat local offset (`localFrontHorseRidePos` in `d_a_hozelda.cpp`), kept
    // here only so it can be blended against -- outside of a rear this is exactly what vanilla
    // already uses, so blending starts and ends at the same place vanilla would have placed her.
    static const Vec kZeldaFrontHorseRidePos = {-75.893997f, 57.61f, 4.079f};

    // `daAlink_c`'s own local ride offset for Link's solo riding position (`l_localHorseRidePos` in
    // `d_a_alink.cpp`, a file-local constant we can't reference directly). Blending toward it in
    // place of Zelda's own, further-forward/higher offset is the whole point of the fix: it's the
    // offset Link's own riding position already proves doesn't clip or float during a rear.
    static const Vec kLinkHorseRidePos = {-68.208984f, 41.609924f, 0.883789f};

    Vec blendedPos = {
        kZeldaFrontHorseRidePos.x +
            (kLinkHorseRidePos.x - kZeldaFrontHorseRidePos.x) * s_turnStandBlend,
        kZeldaFrontHorseRidePos.y +
            (kLinkHorseRidePos.y - kZeldaFrontHorseRidePos.y) * s_turnStandBlend,
        kZeldaFrontHorseRidePos.z +
            (kLinkHorseRidePos.z - kZeldaFrontHorseRidePos.z) * s_turnStandBlend,
    };

    mDoMtx_multVec(horse->getRootMtx(), &blendedPos, &zelda->current.pos);
    zelda->shape_angle = horse->shape_angle;
    zelda->current.angle.y = zelda->shape_angle.y;

    mDoMtx_stack_c::transS(zelda->current.pos);
    mDoMtx_stack_c::ZXYrotM(zelda->shape_angle.x, zelda->shape_angle.y, zelda->shape_angle.z);
    zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
}

// --- Auto-targeting nearby enemies with Light Arrows ---------------------------------------
//
// Vanilla's `daHoZelda_c` (the horseback-Zelda actor) only ever draws her bow at one specific
// actor: `mGndAcKeep` is populated exclusively by `daHoZelda_searchGanon()`, a search filter
// hardcoded to Ganondorf (`fpcNm_B_GND_e`). `execute()` searches for him once per tick whenever
// `mGndAcKeep` is currently empty; `setAnm()` then only ever enters her bow-draw state machine
// (`mBowMode`) while the *player's own* Z-target list is locked onto that same actor and
// `b_gnd_class::checkPiyo()` (a Ganondorf-only "is he currently staggered" accessor) says he isn't;
// `searchBodyAngle()` (called right after `setAnm()` by `execute()`) aims at whatever
// `mGndAcKeep.getActor()` happens to be, generically, via its `eyePos` -- it has no Ganondorf
// -specific logic at all.
//
// So two changes make her auto-fire at nearby enemies instead of just Ganondorf:
//  1. A pre-hook on `execute()` feeds our own nearest-enemy search into `mGndAcKeep` in place of
//     Ganondorf, scoped to our own spawned Zelda only. Since `execute()`'s own
//     `mGndAcKeep.setActor()` (right after this pre-hook runs) only *validates* whatever actor
//     `mGndAcKeep` already holds by ID -- it doesn't clear a still-alive actor -- our target
//     survives that call, and vanilla's own Ganondorf search is skipped entirely (it only runs
//     when `mGndAcKeep.getActor() == NULL`). `searchBodyAngle()` then aims at our target with no
//     further changes needed.
//  2. A replace hook on `setAnm()` swaps out the player-lock-on and `checkPiyo()` requirements
//     (meaningless -- and, for `checkPiyo()`, unsafe to even call on a non-Ganondorf actor, since
//     it reads a `b_gnd_class`-specific field through a cast that would otherwise be reading
//     unrelated memory for any other enemy type) for a generic "does our target exist and have
//     health left" check, and drops the real duel's requirement that Link himself be riding double
//     (`checkHorseRide()`), since this mod's own Zelda rides solo. Every other branch of that
//     function -- the horse-anim-to-Zelda-anim mapping, the upper-body bow draw/nock/release state
//     machine, the arrow actor itself -- is an unmodified copy of vanilla's own logic, since none
//     of it depends on the target being Ganondorf specifically. Light Arrows already deal normal
//     damage (and use their normal hit effects) against any ordinary enemy as-is: the shared hit
//     -resolution code (`cc_at_check()` in `d_cc_uty.cpp`) only zeroes out an arrow's light-based
//     bonus damage for Ganondorf himself (whose real damage/stagger is handled separately) and
//     Zant; no new damage type or visual effect is needed for this.
//
// Scoped entirely to our own spawned Zelda: the real scripted duel always uses its own,
// story-placed HoZelda actor, which is left completely untouched by both hooks below.

// How far (in game units, matching e.g. `mpHIO->m.bow_end_distance`'s unused 4000.0f default) and
// within what cone (reusing `mpHIO->m.bow_end_angle`, the same wider "keep firing" cone vanilla
// already uses once her bow is drawn) our own Zelda will look for a new target.
static const f32 kAutoTargetRange = 4000.0f;

struct HoZeldaTargetSearch {
    const fopAc_ac_c* self;
    f32 maxDistSq;
    s16 maxAngle;
    fopAc_ac_c* best;
    f32 bestDistSq;
};

// `fopAcIt_Judge()` stops and returns at the first non-NULL result, so to find the *nearest*
// enemy (rather than just the first one in the actor list) this always returns NULL -- keeping
// the iteration going over every actor -- and instead threads the closest candidate so far through
// `i_data`, read back once `fopAcIt_Judge()` itself returns.
static void* judge_nearest_enemy(fopAc_ac_c* i_actor, void* i_data) {
    HoZeldaTargetSearch* search = static_cast<HoZeldaTargetSearch*>(i_data);

    if (i_actor == search->self || fopAcM_GetGroup(i_actor) != fopAc_ENEMY_e ||
        fopAcM_GetName(i_actor) == fpcNm_B_GND_e || i_actor->health <= 0)
    {
        // Ganondorf is deliberately excluded: he already has his own, correctly-functioning
        // targeting during the real duel (handled by vanilla's untouched Ganondorf search when
        // this mod's hooks aren't in play for his own story-placed Zelda), and auto-targeting him
        // outside of that scripted fight would conflict with it.
        return NULL;
    }

    f32 distSq = search->self->current.pos.abs2XZ(i_actor->current.pos);
    if (distSq > search->maxDistSq ||
        fopAcM_seenActorAngleY(search->self, i_actor) > search->maxAngle)
    {
        return NULL;
    }

    if (search->best == NULL || distSq < search->bestDistSq) {
        search->best = i_actor;
        search->bestDistSq = distSq;
    }

    return NULL;
}

static fopAc_ac_c* find_nearest_enemy(daHoZelda_c* zelda) {
    HoZeldaTargetSearch search = {};
    search.self = zelda;
    search.maxDistSq = kAutoTargetRange * kAutoTargetRange;
    search.maxAngle = zelda->mpHIO->m.bow_end_angle;
    fopAcIt_Judge((fopAcIt_JudgeFunc)judge_nearest_enemy, &search);
    return search.best;
}

// Pre-hook for change (1) above: see the overview comment further up.
DEFINE_HOOK(&daHoZelda_c::execute, HoZeldaExecute);

static HookAction on_hozelda_execute_pre(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda)) {
        return HOOK_CONTINUE;
    }

    if (auto_target_enemies_enabled()) {
        zelda->mGndAcKeep.setData(find_nearest_enemy(zelda));
    } else {
        // Toggled off (possibly mid-ride): drop whatever target we'd previously picked so the
        // function below naturally falls back to doing nothing, the same as if she'd never found
        // anyone.
        zelda->mGndAcKeep.clearData();
    }
    return HOOK_CONTINUE;
}

// Replace hook for change (2) above: a copy of vanilla's `daHoZelda_c::setAnm()` (dusklight's
// `src/d/actor/d_a_hozelda.cpp`) for our own spawned Zelda only, with just the Ganondorf-specific
// targeting checks replaced (search the body for "deviates from vanilla" below); every other
// branch is unmodified. Operates on `zelda->member` throughout, rather than `this->member`, since
// it isn't itself a member function.
static void set_anm_auto_target(daHoZelda_c* zelda) {
    u16 anm_idx[3];
    daHorse_c* horse = (daHorse_c*)dComIfGp_getHorseActor();
    daPy_py_c* player = daPy_getLinkPlayerActorClass();

    if (horse == NULL) {
        return;
    }

    u16* anm_p = anm_idx;
    for (int i = 0; i < 3; i++, anm_p++) {
        u16 horse_anm_idx = horse->getAnmIdx(i);
        if (horse_anm_idx == 0x14) {
            *anm_p = 0x16;
        } else if (horse_anm_idx == 0x15) {
            *anm_p = 0x18;
        } else if (horse_anm_idx == 0x16) {
            *anm_p = 0x19;
        } else if (horse_anm_idx == 0xC) {
            *anm_p = 0x14;
        } else if (horse_anm_idx == 0xB) {
            *anm_p = 0x12;
        } else if (horse_anm_idx == 0xA) {
            *anm_p = 0x13;
        } else if (horse_anm_idx == 0x11 || horse_anm_idx == 0x12 || horse_anm_idx == 0x13) {
            // EXPERIMENTAL (for testing only): vanilla keeps RUN_DASH (0x11) mapped to its own
            // Zelda anim (0xF), which is never paired with the bow pose, so Zelda could only ever
            // draw her bow at a full gallop (RUN_FAST/RUN_SLOW). Folding dash into the same bucket
            // as gallop (0xE) makes the bow-draw gate below reachable while dashing too, so the
            // user can play-test how it looks/feels at more than one gait.
            *anm_p = 0xE;
        } else if (horse_anm_idx == 0xFFFF) {
            *anm_p = 0xFFFF;
        } else {
            *anm_p = 0x1C;
        }
    }

    // --- Deviates from vanilla from here... ---
    fopAc_ac_c* target_actor = zelda->mGndAcKeep.getActor();

    // Only non-NULL if the target this mod picked happens to actually be Ganondorf (never the
    // case in practice: `judge_nearest_enemy()` above excludes him), kept so his own mount/
    // vulnerability checks below still apply correctly rather than silently skipping them if it
    // ever does happen.
    b_gnd_class* ganondorf = (target_actor != NULL && fopAcM_GetName(target_actor) == fpcNm_B_GND_e)
                                 ? static_cast<b_gnd_class*>(target_actor)
                                 : NULL;

    int target_seen_angleY = 0;
    if (target_actor != NULL) {
        target_seen_angleY = fopAcM_seenActorAngleY(zelda, target_actor);
    }

    // Vanilla requires `b_gnd_class::checkPiyo()` (only meaningful, and only safe to call, on an
    // actual Ganondorf actor) and the player's own Z-target list to equal the target. Replaced
    // here with a generic "does it still have health left" check and an always-on auto-lock, since
    // this mod's Zelda has no player of her own to lock on for her.
    bool target_vulnerable = false;
    if (ganondorf != NULL) {
        target_vulnerable = ganondorf->checkPiyo() != 1;
    } else if (target_actor != NULL) {
        target_vulnerable = target_actor->health > 0;
    }

    // EXPERIMENTAL (for testing only): vanilla only ever reaches this point with anm_idx[0] == 0xE
    // (full gallop) or 0x1C (the catch-all bucket for every other gait the mapping above doesn't
    // special-case: walk, trot/turn, idle/wait, excitement, etc. -- see the mapping loop above).
    // Both values are safe to allow here since the overlay-blend branch further below (the
    // `anm_idx[0] != 0x1C && anm_idx[0] != 0xE` check) already treats them identically to gallop,
    // so this just widens which gaits can trigger the bow-draw state machine for play-testing.
    if ((anm_idx[0] == 0xE || anm_idx[0] == 0x1C) && zelda->field_0x6da == 0 && !zelda->mDamageInit &&
        zelda->field_0x6dd == 0 && target_actor != NULL && target_vulnerable &&
        (target_seen_angleY < zelda->mpHIO->m.bow_start_angle ||
            (zelda->mBowMode != 0 && target_seen_angleY < zelda->mpHIO->m.bow_end_angle)))
    {
        zelda->mBowMode = 1;
    } else {
        zelda->mBowMode = 0;
    }
    // --- ...to here; everything below is an unmodified copy of vanilla. ---

    int sp28 = 1;
    int sp24 = 0;
    daPy_frameCtrl_c* frame_ctrl = &zelda->mFrameCtrl[1];
    zelda->mIsSingleRide = false;

    if (!player->checkHorseRide() && (ganondorf == NULL || ganondorf->checkRide() != 1)) {
        if (anm_idx[0] == 0x16) {
            anm_idx[0] = 0x17;
        } else if (ganondorf != NULL && ganondorf->checkZeldaEndDemoCut()) {
            anm_idx[0] = 0x11;
            sp28 = 0;
        } else {
            anm_idx[0] = 0x1B;
            sp28 = 0;
        }

        zelda->mIsSingleRide = true;
    } else if (dComIfGp_checkPlayerStatus0(0, 0x20000000)) {
        anm_idx[0] = 7;
        sp28 = 0;
        zelda->field_0x6dd = 1;
        sp24 = 1;
    } else if (player->getDemoMode() == 0x5D) {
        anm_idx[0] = 0x10;
        sp28 = 0;
        zelda->field_0x6dd = 1;
    }

    if ((anm_idx[0] != 0x1C && anm_idx[0] != 0xE) || player->checkHorseElecDamage()) {
        zelda->resetUpperAnime();
        if (zelda->field_0x6e4[0] != anm_idx[0]) {
            zelda->setSingleAnime(anm_idx[0], 1.0f, 0.0f, -1, horse->getMorfFrame());
            if (zelda->field_0x6dd != 0) {
                if (sp24 != 0) {
                    zelda->setEyeBtp(0x2C);
                    zelda->setEyeBtk(0x27, 1);
                } else {
                    zelda->setEyeBtp(0x2E);
                    zelda->setEyeBtk(0x28, 1);
                }
            }
        }

        zelda->deleteArrow();
    } else if (zelda->mBowMode != 0) {
        BOOL anm_end = zelda->mFrameCtrl[2].checkAnmEnd();
        anm_idx[0] = 0x1A;
        anm_idx[2] = 0xFFFF;

        if (zelda->mUpperAnmID == 9) {
            if (anm_end) {
                anm_idx[2] = 0x1A;
                zelda->mAnmTimer = 30;
            }
        } else if (zelda->mUpperAnmID == 0xA) {
            if (zelda->mAnmTimer == 0) {
                anm_idx[2] = 9;
                zelda->setBowBck(0xC);
                zelda->shootArrow();
                zelda->mSound.startCreatureSound(Z2SE_ZELDA_ARROW_SHOT, 0, zelda->mReverb);
            } else {
                zelda->mSound.startCreatureSoundLevel(Z2SE_ZELDA_ARROW_READY, 0, zelda->mReverb);
            }
        } else if (zelda->mUpperAnmID == 8) {
            zelda->mSound.startCreatureSoundLevel(Z2SE_ZELDA_ARROW_READY, 0, zelda->mReverb);
            if (anm_end) {
                anm_idx[2] = 0xA;
                zelda->mAnmTimer = 30;
            }
        } else if (zelda->mUpperAnmID == 0x1A) {
            if (zelda->mAnmTimer == 0) {
                zelda->mArrowAcKeep.setData(daArrow_c::makeArrow(zelda, 2));
                if (zelda->mArrowAcKeep.getActor() != NULL) {
                    anm_idx[2] = 8;
                    zelda->setBowBck(0xB);
                    zelda->mSound.startCreatureSound(Z2SE_ZELDA_ARROW_DRAW, 0, zelda->mReverb);
                }
            }
        } else {
            anm_idx[2] = 0x1A;
            zelda->mAnmTimer = 0;
        }

        if (anm_idx[2] != 0xFFFF) {
            zelda->setUpperAnime(anm_idx[2]);
            zelda->setSingleAnime(anm_idx[0], 1.0f, 0.0f, -1, 4.0f);
        }
    } else {
        if (zelda->field_0x6da == 0 && zelda->mDamageInit) {
            zelda->field_0x6da = 1;
            zelda->mBowMode = 0;
            zelda->setUpperAnime(0xD);
            zelda->setEyeBtp(0x2D);
            zelda->setEyeBtk(0x26, 0);
        } else {
            if (zelda->field_0x6dd != 0) {
                zelda->field_0x6dd = 0;
                zelda->setNormalFace();
            }

            if (zelda->mBowMode != 0 ||
                (zelda->field_0x6da != 0 && zelda->mFrameCtrl[2].checkAnmEnd()))
            {
                zelda->resetUpperAnime();
            }
        }

        if (anm_idx[1] == 0xFFFF) {
            anm_idx[1] = anm_idx[0];
        }

        if (zelda->field_0x6e4[1] != anm_idx[1] || zelda->field_0x6e4[0] != anm_idx[0]) {
            zelda->setDoubleAnime(
                horse->getBlendRate(), 1.0f, 1.0f, anm_idx[0], anm_idx[1], horse->getMorfFrame());
        }

        if (zelda->field_0x6e4[0] != 0xE) {
            sp28 = 0;
        }

        if (zelda->field_0x6e4[1] == 0xE) {
            frame_ctrl->setFrame(horse->getAnmFrame(1));
            zelda->mAnmRatioPack[1].getAnmTransform()->setFrame(frame_ctrl->getFrame());
        }

        zelda->deleteArrow();
    }

    if (sp28 != 0) {
        zelda->mFrameCtrl[0].setFrame(horse->getAnmFrame(0));
        zelda->mAnmRatioPack[0].getAnmTransform()->setFrame(zelda->mFrameCtrl[0].getFrame());

        if (zelda->field_0x6e4[1] == 0x1A) {
            frame_ctrl->setFrame(zelda->mFrameCtrl[0].getFrame());
            zelda->mAnmRatioPack[1].getAnmTransform()->setFrame(frame_ctrl->getFrame());
        }
    }
}

DEFINE_HOOK(&daHoZelda_c::setAnm, HoZeldaSetAnm);

static void on_hozelda_set_anm_replace(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda) || !auto_target_enemies_enabled()) {
        HoZeldaSetAnm::g_orig(zelda);
        return;
    }

    set_anm_auto_target(zelda);
}

// The Light Arrow actor (`daArrow_c`) never actually aims itself at Ganondorf, or at anything:
// `arrowShooting()` only overrides its flight angle/position when the *player* is actively using
// their own bow-camera aim (`checkBowCameraArrowPosP()`), which is never the case for either the
// real duel (the player presses the action button while locked on; Link never equips/aims his own
// bow there) or for the mod's auto-firing Zelda. With that check never satisfied, the arrow simply
// keeps flying in whatever direction Zelda's hand was last facing per-frame via `setKeepMatrix()`
// right up until release -- which itself only ever turns a few degrees off of her forward-facing
// body thanks to `searchBodyAngle()`'s very narrow `bow_search_y_angle` clamp (a handful of
// degrees). None of this was ever a problem for the real duel, since Ganondorf is always choreographed
// to stay almost directly ahead of her the whole time; it just never had to aim any wider than that.
// Once real, potentially off-to-the-side enemies are in play, that narrow a correction isn't enough,
// and arrows end up flying essentially straight ahead regardless of where the target actually is.
//
// Rather than trying to widen that narrow vanilla clamp (which also drives her visible body/neck
// twist, and isn't under this mod's control without its own replace hook on `searchBodyAngle()`),
// this re-aims the arrow directly at the moment it's released: for arrows fired by our own Zelda,
// the flight direction/speed vanilla would have left untouched is instead pointed straight at
// whatever enemy she's currently tracking, using the same generic hit-resolution/visuals every
// other arrow already uses -- only the direction changes.
DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);

static void on_arrow_shooting_post(ModContext*, void* args, void*, void*) {
    daArrow_c* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow->mArrowType != daArrow_c::ARROW_TYPE_LIGHT || !auto_target_enemies_enabled()) {
        return;
    }

    daHoZelda_c* zelda = (daHoZelda_c*)arrow->field_0xa08.getActor();
    if (zelda == nullptr || fopAcM_GetName(zelda) != fpcNm_HOZELDA_e || !is_spawned_zelda(zelda)) {
        return;
    }

    fopAc_ac_c* target = zelda->mGndAcKeep.getActor();
    if (target == nullptr) {
        return;
    }

    cXyz dir = target->eyePos - arrow->current.pos;
    if (dir.abs() < 1.0f) {
        return;
    }

    // Matches the sign/axis conventions vanilla's own `arrowShooting()` and `searchBodyAngle()`
    // already use elsewhere in this file for converting a world-space direction into the engine's
    // pitch/yaw angle pair, so the arrow's visible orientation while flying stays consistent with
    // every other arrow.
    arrow->current.angle.x = -dir.atan2sY_XZ();
    arrow->current.angle.y = dir.atan2sX_Z();
    arrow->shape_angle.y = arrow->current.angle.y;

    dir.normalizeZP();
    arrow->speed = dir * arrow->field_0x99c;
}

// Rather than attempting to patch up Zelda's orientation, animation, and any overlapping props for
// every individual story cutscene that drives Epona (a reaction/firewood/etc. fix was tried for
// each as they were found, but kept surfacing new, similarly-themed regressions with no end in
// sight), this mod instead hides its own spawned Zelda for the duration of any such cutscene by
// default, controlled by `g_cvarShowInCutscenes` above (see `mod_update()`'s cutscene-detection
// gate). The orientation fix above still applies whenever she *is* shown (including when the user
// opts in via that toggle), since that's a simple, narrowly-scoped correction to her own seat, not
// a per-cutscene patch.
static ModResult build_mods_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Show Zelda during cutscenes";
    control.help_rml = "When off (default), Zelda is hidden for the duration of scripted story "
                        "cutscenes and reappears once they end. Cutscenes that already feature "
                        "their own story-placed Zelda, and the horse call/grass whistle, are "
                        "unaffected either way.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarShowInCutscenes;
    svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);

    UiControlDesc autoTargetControl = UI_CONTROL_DESC_INIT;
    autoTargetControl.kind = UI_CONTROL_TOGGLE;
    autoTargetControl.label = "Auto-target nearby enemies with Light Arrows";
    autoTargetControl.help_rml = "When on (default), Zelda automatically draws her bow and fires "
                                  "Light Arrows at nearby enemies, the same way she already does "
                                  "at Ganondorf during the horseback duel. When off, she just rides "
                                  "along passively.";
    autoTargetControl.binding = UI_BINDING_CONFIG_VAR;
    autoTargetControl.config_var = g_cvarAutoTargetEnemies;
    svc_ui->pane_add_control(mod_ctx, panel, &autoTargetControl, nullptr);
    return MOD_OK;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = mods::hook::add_pre<HorseCallSubstance>(on_horse_call_substance_pre);
    if (result != MOD_OK) {
        mods::log::warn("failed to hook horse call, grass whistle may behave oddly: {}",
                         (int)result);
        // Not fatal: the mod still works, just without the horse-call fix.
    }

    result = mods::hook::add_post<HoZeldaSetMatrix>(on_hozelda_set_matrix_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda matrix, orientation may be wrong during some cutscenes: {}",
            (int)result);
        // Not fatal: the mod still works, just without the cutscene-orientation fix.
    }

    result = mods::hook::add_pre<HoZeldaExecute>(on_hozelda_execute_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda execute, auto-targeting enemies will be unavailable: {}",
            (int)result);
        // Not fatal: the mod still works, Zelda just won't auto-target enemies.
    }

    result = mods::hook::replace<HoZeldaSetAnm>(on_hozelda_set_anm_replace);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda setAnm, auto-targeting enemies will be unavailable: {}",
            (int)result);
        // Not fatal: the mod still works, Zelda just won't auto-target enemies.
    }

    result = mods::hook::add_post<ArrowShooting>(on_arrow_shooting_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook arrow shooting, auto-targeted Light Arrows may fly straight instead "
            "of toward their target: {}",
            (int)result);
        // Not fatal: the mod still works, arrows just won't be re-aimed at the target.
    }

    ConfigVarDesc cvarDesc = CONFIG_VAR_DESC_INIT;
    cvarDesc.name = "showInCutscenes";
    cvarDesc.type = CONFIG_VAR_BOOL;
    cvarDesc.default_bool = false;
    result = svc_config->register_var(mod_ctx, &cvarDesc, &g_cvarShowInCutscenes);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to register showInCutscenes option, defaulting to hidden during cutscenes: {}",
            (int)result);
        // Not fatal: `show_zelda_in_cutscenes()` already falls back to "off" when the var isn't
        // registered.
    }

    ConfigVarDesc autoTargetCvarDesc = CONFIG_VAR_DESC_INIT;
    autoTargetCvarDesc.name = "autoTargetEnemies";
    autoTargetCvarDesc.type = CONFIG_VAR_BOOL;
    autoTargetCvarDesc.default_bool = true;
    result = svc_config->register_var(mod_ctx, &autoTargetCvarDesc, &g_cvarAutoTargetEnemies);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to register autoTargetEnemies option, defaulting to auto-targeting on: {}",
            (int)result);
        // Not fatal: `auto_target_enemies_enabled()` already falls back to "on" when the var
        // isn't registered.
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_mods_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    if (result != MOD_OK) {
        mods::log::warn("failed to register mods panel, the cutscene toggle won't be visible: {}",
                         (int)result);
        // Not fatal: the config var still exists (and can be set via config.json/--cvar even
        // without a UI control for it), it just won't be reachable from the Mods window.
    }

    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daHorse_c* horse = dComIfGp_getHorseActor();

    if (horse == nullptr) {
        // No horse actor is currently loaded (e.g. a different area, or before Epona is tamed).
        // `daHoZelda_c` never deletes itself (it has no `is_delete` method), so if we don't
        // explicitly delete the actor we spawned here it would keep existing and executing
        // forever, orphaned from any horse. That leaked actor is a likely cause of the
        // "two overlapping Zelda models" bug: once a new horse actor appears later, we'd spawn
        // a second HoZelda while the first, orphaned one is still alive and animating.
        remove_spawned_zelda();
        return MOD_OK;
    }

    // Refresh whether the actor we previously spawned is still alive (it may have been deleted
    // by the game for reasons outside our control, e.g. a scene change).
    fopAc_ac_c* trackedZelda = find_spawned_zelda();

    // Diagnostic: if our tracked actor is still alive but isn't the one currently attached to the
    // horse, something else (the story, or another spawn we lost track of) has its own HoZelda
    // riding at the same time as ours — i.e. exactly the "two overlapping Zelda models" bug. This
    // should never happen given the checks below, but if it does, logging it (with both actors'
    // IDs) is the best lead available for further diagnosis without being able to run the game.
    fopAc_ac_c* attachedZelda = horse->getZeldaActor();
    if (trackedZelda != nullptr && attachedZelda != nullptr && attachedZelda != trackedZelda) {
        mods::log::warn(
            "horse has HoZelda actor id {} attached, but our own HoZelda actor id {} is still "
            "alive and unattached -- two Zelda actors may be visible at once",
            (int)fopAcM_GetID(attachedZelda), (int)s_spawnedZeldaId);
    }

    // Whether a real, scripted story cutscene -- as opposed to ordinary gameplay -- is currently
    // driving Epona. `daHorse_c::procToolDemo()` (`m_procID == PROC_TOOL_DEMO_e`) is the state the
    // horse enters only while being puppeted directly by JStudio demo data
    // (`dDemo_c::getActor()`), which is exactly how every scripted story cutscene moves and
    // animates her; ordinary player-driven riding, and the horse-call/grass-whistle gallop-back
    // (which repurposes some of the same demo-mode plumbing for its own unrelated "run to player"
    // behavior, flagged by `FLG0_CALL_HORSE` instead), never set it.
    bool inScriptedCutscene = horse->m_procID == daHorse_c::PROC_TOOL_DEMO_e &&
                              !horse->checkStateFlg0(daHorse_c::FLG0_CALL_HORSE);

    if (inScriptedCutscene) {
        // Default behavior: don't intrude on story cutscenes with our own spawned Zelda unless the
        // user opts in via the "Show Zelda during cutscenes" toggle. This only ever affects our
        // own spawned actor -- cutscenes with their own story-placed HoZelda (e.g. the Ganondorf
        // duel) never have `s_hasSpawnedZelda` set in the first place, so they're untouched either
        // way.
        if (!show_zelda_in_cutscenes()) {
            remove_spawned_zelda();
        }
        // Note: we deliberately never spawn here, even if the toggle is (or just became, via the
        // user flipping it mid-cutscene) true. `fopAcM_create` is not safe to call while
        // `daHorse_c::procToolDemo()` is actively puppeting the horse from scripted JStudio demo
        // data; doing so crashed (SIGABRT) rather than simply creating the actor a frame late. If
        // she isn't already attached by the time a cutscene starts, she stays hidden for its
        // duration and only (re)spawns, below, once the cutscene actually ends.
    } else if (horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda) {
        // Keep Zelda riding along on Epona at all times during ordinary gameplay, even after Link
        // dismounts: if nobody (neither the story nor this mod) currently has her attached to the
        // horse, spawn her. `daHoZelda_c::execute()` attaches itself to the current horse every
        // tick (`horse->setZeldaActor(this)`), and `daHoZelda_c::setMatrix()`/`setRideOffset()`/
        // `setAnm()` already support her sitting alone at the front of the saddle whenever Link
        // isn't riding (the `mIsSingleRide` case), so once created it takes care of the rest on its
        // own: reins, dual-ride animation blending, and the solo idle animation.
        spawn_zelda_on_horse(horse);
    }

    // `daHoZelda_c::setMatrix()` unconditionally calls `onHorseZelda()` every tick whenever any
    // HoZelda actor rides the current horse, marking Link's `FLG2_HORSE_ZELDA` player flag. The
    // game uses that flag to recognize the scripted horseback duel against Ganondorf, and while
    // it is set, several ordinary systems are deliberately disabled for the duration of that
    // fight: dismounting (`checkSpecialHorseRide()` in `daAlink_c::checkHorseGetOffAndSetDoStatus`
    // suppresses `BUTTON_STATUS_DISMOUNT`) and talking to/checking in with Midna
    // (`daAlink_c::orderZTalk()` bails out early whenever `checkHorseZelda()` is true).
    //
    // Outside of that real story duel, this mod is the only thing keeping Zelda mounted, so we
    // clear the flag right back every tick that our own actor (never a story-placed one) is the
    // rider: since `setMatrix()` re-asserts it once per frame too, this turns into a one-flag
    // tug-of-war that is won by whichever side runs last before the player's own logic reads it,
    // but because we clear it every single tick, the flag is never left set for more than the
    // single frame in which `daHoZelda_c` re-asserts it, and it is always false again by the time
    // the player's dismount/Midna checks run on the next tick. A real scripted duel always places
    // its own HoZelda actor (never ours), so this never touches the flag during the actual fight.
    fopAc_ac_c* currentZelda = horse->getZeldaActor();
    if (s_hasSpawnedZelda && currentZelda != nullptr &&
        (ActorId)fopAcM_GetID(currentZelda) == s_spawnedZeldaId) {
        daPy_getLinkPlayerActorClass()->offHorseZelda();
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    remove_spawned_zelda();
    mods::hook::uninstall<HorseCallSubstance>();
    mods::hook::uninstall<HoZeldaSetMatrix>();
    mods::hook::uninstall<HoZeldaExecute>();
    mods::hook::uninstall<HoZeldaSetAnm>();
    mods::hook::uninstall<ArrowShooting>();
    return MOD_OK;
}
}
