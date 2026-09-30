#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/log.hpp"

// Game includes
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ActorService, svc_actor);

// The stage name used by the game to load the horseback-Zelda actor (see `d_stage.cpp`'s
// OBJNAME table: OBJNAME("HoZelda", fpcNm_HOZELDA_e, -1)).
static const char* kHoZeldaStageName = "HoZelda";

// The ActorId of the horseback-Zelda actor this mod spawned, if any. We only ever manage an
// actor we spawned ourselves; a HoZelda actor placed by the vanilla game (e.g. during the
// horseback archery duel against Ganondorf, where Zelda rides alone) is always left untouched.
static ActorId s_spawnedZeldaId = 0;
static bool s_hasSpawnedZelda = false;

// Number of `mod_update` ticks to let pass before attempting our first actor spawn. Enabling (or
// reloading) a mod applies "between frames" (see Dusklight's modding docs, Runtime Lifecycle), so
// `mod_initialize`/the very first `mod_update` call can land at a point in the engine's frame
// outside its normal per-frame actor loop. Calling `fopAcM_create` (which relies on per-frame
// framework state, e.g. the "current layer" the process framework is iterating) from that point
// is suspected to be the cause of a crash observed specifically when enabling the mod while Epona
// is already loaded in the scene (i.e. whenever our very first `mod_update` call would otherwise
// try to spawn immediately). Waiting a few ordinary frames first means our first spawn attempt
// happens from a normal, well-defined point in the frame loop, same as every later spawn (e.g.
// after a scene change) that does not crash.
static const int kWarmupFrames = 30;
static int s_framesSinceEnable = 0;

static fopAc_ac_c* find_spawned_zelda() {
    if (!s_hasSpawnedZelda) {
        return nullptr;
    }

    fopAc_ac_c* actor = nullptr;
    fopAcM_SearchByID((fpc_ProcID)s_spawnedZeldaId, &actor);
    if (actor == nullptr) {
        // The actor we spawned no longer exists (e.g. deleted on a room change).
        s_hasSpawnedZelda = false;
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

    mods::log::info("spawning HoZelda actor in room {} at ({:.1f}, {:.1f}, {:.1f})", params.room_num,
                     params.position.x, params.position.y, params.position.z);

    ActorId newId = 0;
    ModResult result = svc_actor->create_actor_from_name(mod_ctx, kHoZeldaStageName, &params, &newId);
    if (result != MOD_OK) {
        mods::log::warn("failed to spawn HoZelda actor: {}", (int)result);
        return;
    }

    mods::log::info("spawned HoZelda actor id {}", newId);

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

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    // See `kWarmupFrames` above: skip actor-spawning logic entirely for the first several frames
    // after enabling, so our first spawn attempt happens from a normal per-frame context rather
    // than the "between frames" point where the mod was just enabled.
    if (s_framesSinceEnable < kWarmupFrames) {
        s_framesSinceEnable++;
        return MOD_OK;
    }

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
    find_spawned_zelda();

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

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    remove_spawned_zelda();
    return MOD_OK;
}
}
