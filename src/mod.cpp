#include "mods/service.hpp"
#include "mods/svc/actor.h"
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

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daPy_py_c* player = daPy_getPlayerActorClass();
    daHorse_c* horse = dComIfGp_getHorseActor();

    if (player == nullptr || horse == nullptr) {
        // No horse actor exists at all (e.g. still on the title/file-select screens). If we
        // previously spawned a passenger, its horse is long gone, so forget about it.
        s_hasSpawnedZelda = false;
        return MOD_OK;
    }

    bool riding = player->checkHorseRide();
    fopAc_ac_c* ourZelda = find_spawned_zelda();

    if (!riding) {
        // Link dismounted: remove the passenger we added, if any. A vanilla HoZelda actor
        // (e.g. the horseback archery duel where Zelda rides alone) is never touched here,
        // since we only track actors created by `spawn_zelda_on_horse`.
        if (ourZelda != nullptr) {
            remove_spawned_zelda();
        }
        return MOD_OK;
    }

    // Link is riding Epona. If nobody (neither the story nor this mod) currently has Zelda
    // riding along, spawn her. `daHoZelda_c::execute()` attaches itself to the current horse
    // every tick (`horse->setZeldaActor(this)`), so once created it takes care of the rest:
    // reins, dual-ride animation blending, etc.
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
