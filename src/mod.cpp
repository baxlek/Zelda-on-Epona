#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

// Game includes
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(HookService, svc_hook);

// `daHoZelda_c::setMatrix()` runs every frame a HoZelda actor is attached to a horse, and
// unconditionally marks the player with `daPy_py_c::onHorseZelda()` (FLG2_HORSE_ZELDA). That
// flag is meant to gate the scripted horseback duel against Ganondorf: it blocks the normal
// dismount action (`checkSpecialHorseRide()`), diverts the grass-whistle horse call into a
// duel-only demo instead of the usual "gallop to player" sequence, and (since dismounting is
// blocked) prevents reaching Wolf Link/Midna. We hook `setMatrix()` to clear the flag back off
// immediately whenever it is *our* spawned Zelda causing it, restoring normal dismounting,
// horse-calling, and Midna access, while leaving a real story-placed HoZelda actor (the actual
// duel) untouched.
DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);

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

static void on_hozelda_set_matrix_post(ModContext*, void* args, void*, void*) {
    if (!s_hasSpawnedZelda) {
        return;
    }

    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    fopAc_ac_c* spawned = find_spawned_zelda();
    if (spawned != nullptr && reinterpret_cast<fopAc_ac_c*>(zelda) == spawned) {
        // Undo the "special horse ride" state setMatrix() just applied for our own Zelda, so
        // dismounting, the grass-whistle horse call, and Midna access keep working normally.
        daPy_getLinkPlayerActorClass()->offHorseZelda();
    }
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult hookResult = mods::hook::add_post<HoZeldaSetMatrix>(on_hozelda_set_matrix_post);
    if (hookResult != MOD_OK) {
        mods::log::warn("failed to hook daHoZelda_c::setMatrix: {}", (int)hookResult);
    }

    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daHorse_c* horse = dComIfGp_getHorseActor();

    if (horse == nullptr) {
        // No horse actor is currently loaded (e.g. a different area, or before Epona is tamed).
        // A HoZelda actor's lifetime is tied to the horse, so forget any stale tracked id.
        s_hasSpawnedZelda = false;
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
    mods::hook::uninstall<HoZeldaSetMatrix>();
    return MOD_OK;
}
}
