#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/log.hpp"

// Game includes
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_mtx.h"

DEFINE_MOD();

IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(LogService, svc_log);

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

// `daHoZelda_c::setMatrix()` orients Zelda by copying `horse->shape_angle` wholesale, a separate,
// logical heading field that is normally kept in lockstep with the horse's own rendered pose
// (`daHorse_c::setMatrix()` builds the horse's own base matrix from that exact same field). During
// ordinary gameplay those always agree. But during story cutscenes driven by
// `daHorse_c::procToolDemo()` (scripted demo data moving/animating Epona directly), `shape_angle`
// is only overwritten when the demo track has its rotation channel enabled
// (`dDemo_actor_c::ENABLE_ROTATE_e`); many cutscenes never enable it (e.g. because vanilla never
// needed Epona's logical heading there, only her rendered pose), leaving `shape_angle` frozen at a
// stale value (often whatever it was before the cutscene started, which can read as a fixed,
// arbitrary "facing North") while Epona's actual animated pose (including rearing up) keeps
// changing underneath it.
//
// Several attempts at fixing this by instead *decomposing* a joint matrix into Euler angles (via
// `mDoMtx_MtxToRot()`, with some fixed correction for that joint's own bind-pose twist, the same
// technique `daAlink_c::setSyncHorsePos()` uses for Link's own rotation while riding) all
// regressed in one way or another: substituting the root joint wholesale caused a constant ~90
// degree error everywhere; deriving the angle from the saddle joint while leaving position on the
// original's root joint caused Zelda to sink into Epona's model while rearing (root and saddle
// rotate independently enough in that extreme pose for the mismatch to become visible);
// re-anchoring position to the root joint's translation while only re-orienting the local offset
// using the saddle-derived angle caused her to float next to Epona instead; and deriving *both*
// position and angle from the saddle joint together (mirroring Link's own technique exactly)
// moved her noticeably off her normal seat even outside of rearing, because her local seat offset
// constants (`localHorseRidePos`/`localFrontHorseRidePos`) are calibrated specifically against the
// *root* joint in the original, unmodified function -- transforming them through any other joint
// moves the computed point, regardless of how internally consistent that joint's own position and
// rotation are with each other.
//
// The fix that actually keeps both constraints satisfied -- the anchor point must come from the
// root joint (to match the calibrated seat offsets), and position/rotation must never disagree
// about whose pose they describe (to avoid sinking/floating) -- is to stop decomposing into Euler
// angles at all. `horse->getRootMtx()` is a live, fully up-to-date animated joint matrix in every
// situation (ordinary gameplay and demo-driven cutscenes alike; only the separate `shape_angle`
// field goes stale, never the joint matrices themselves). Copying it verbatim and translating by
// the local seat offset in *its own* local frame reproduces the exact same point the original,
// unmodified function computes (root's translation plus root's rotation applied to the offset),
// while also using that same matrix's own rotation -- whatever bind-pose convention it carries --
// to draw Zelda's mesh. Since position and the drawn rotation both come from one single,
// un-decomposed matrix, they can never disagree with each other, in any pose, including rearing.
//
// Scoped to our own spawned Zelda only: the vanilla duel's story-placed HoZelda already works
// correctly as-is (it's the one case this engine code was originally written for), so it's left
// untouched to avoid any risk of regressing it.
DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);

static void on_hozelda_set_matrix_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!s_hasSpawnedZelda || (ActorId)fopAcM_GetID(zelda) != s_spawnedZeldaId) {
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || zelda->model == nullptr) {
        return;
    }

    // Build the final matrix directly from the root joint's own live matrix instead of
    // decomposing/rebuilding via Euler angles (see the explanatory comment above `DEFINE_HOOK`).
    // `transM` concatenates a local-frame translation onto `now` (`now = now * T(local_pos)`),
    // which reproduces exactly the same point the original, unmodified function computes via
    // `mDoMtx_multVec(horse->getRootMtx(), local_pos, &current.pos)` -- root's translation plus
    // root's own rotation applied to the offset -- while keeping that same rotation for the drawn
    // matrix, so the two can never disagree.
    static const Vec localHorseRidePos = {-5.894f, 52.61f, 4.079f};
    static const Vec localFrontHorseRidePos = {-75.893997f, 57.61f, 4.079f};
    const Vec* local_pos = daPy_getLinkPlayerActorClass()->checkHorseRide() ? &localHorseRidePos
                                                                             : &localFrontHorseRidePos;

    mDoMtx_stack_c::copy(horse->getRootMtx());
    mDoMtx_stack_c::transM(local_pos->x, local_pos->y, local_pos->z);
    MtxP finalMtx = mDoMtx_stack_c::get();

    cXyz pos(finalMtx[0][3], finalMtx[1][3], finalMtx[2][3]);
    zelda->current.pos = pos;

    // `shape_angle` is still decomposed (best-effort, no bind-pose correction) purely to keep
    // Zelda's logical heading field populated for any other game logic that reads it; it has no
    // effect on how she's actually drawn, since `finalMtx` above is used for that directly.
    csXyz rot;
    mDoMtx_MtxToRot(finalMtx, &rot);
    zelda->shape_angle = rot;
    zelda->current.angle.y = rot.y;

    zelda->model->setBaseTRMtx(finalMtx);
}

// The early "Ordon Bundle" story cutscene (`daObjToaruMaki_c`, stage name "T_Maki") shows Epona
// carrying firewood as a separate decorative prop placed directly at the saddle position for that
// one shot, not as part of Epona's own model. It's entirely unrelated to this mod and always
// present regardless of HoZelda, so with this mod keeping Zelda spawned on Epona at all times, the
// two end up occupying the same spot. Since the bundle is static set-dressing (it never tracks the
// horse itself, just redraws at its own fixed placement every tick), the only way to resolve the
// overlap is to remove it outright whenever it's actually coincident with the horse; a generous
// radius still leaves unrelated decorative bundles placed elsewhere in the game untouched.
static constexpr f32 kFirewoodOverlapRadius = 300.0f;

static void* find_overlapping_firewood_bundle(fopAc_ac_c* i_actor, void* i_data) {
    if (fopAcM_GetName(i_actor) != fpcNm_Obj_ToaruMaki_e) {
        return nullptr;
    }

    daHorse_c* horse = static_cast<daHorse_c*>(i_data);
    if (i_actor->current.pos.abs2(horse->current.pos) >
        kFirewoodOverlapRadius * kFirewoodOverlapRadius) {
        return nullptr;
    }

    return i_actor;
}

static void remove_overlapping_firewood_bundle(daHorse_c* horse) {
    fopAc_ac_c* bundle = (fopAc_ac_c*)fopAcIt_Judge(
        (fopAcIt_JudgeFunc)find_overlapping_firewood_bundle, horse);
    if (bundle != nullptr) {
        svc_actor->delete_actor(mod_ctx, (ActorId)fopAcM_GetID(bundle));
    }
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

    // Keep Zelda riding along on Epona at all times, even after Link dismounts: if nobody
    // (neither the story nor this mod) currently has her attached to the horse, spawn her.
    // `daHoZelda_c::execute()` attaches itself to the current horse every tick
    // (`horse->setZeldaActor(this)`), and `daHoZelda_c::setMatrix()`/`setRideOffset()`/`setAnm()`
    // already support her sitting alone at the front of the saddle whenever Link isn't riding
    // (the `mIsSingleRide` case), so once created it takes care of the rest on its own: reins,
    // dual-ride animation blending, and the solo idle animation.
    if (horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda) {
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

    // See `remove_overlapping_firewood_bundle`'s comment. Deliberately not gated behind
    // `currentZelda == s_spawnedZeldaId` above: during the very cutscene this targets, our Zelda
    // may not have attached to the horse yet on the exact frame the bundle actor loads in (actor
    // creation/attachment can straddle frames, see `find_spawned_zelda()`), so checking every tick
    // the horse exists at all is what actually catches it reliably.
    if (s_hasSpawnedZelda) {
        remove_overlapping_firewood_bundle(horse);
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    remove_spawned_zelda();
    mods::hook::uninstall<HorseCallSubstance>();
    mods::hook::uninstall<HoZeldaSetMatrix>();
    return MOD_OK;
}
}
