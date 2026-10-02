#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

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
// (`dDemo_actor_c::ENABLE_ROTATE_e`); many cutscenes never enable it, leaving `shape_angle` frozen
// at a stale value while Epona's actual animated pose (including rearing up) keeps changing
// underneath it.
//
// Crucially, this only matters for cutscenes where *this mod's* Zelda rides solo (Link not also on
// Epona): vanilla never had a reason to keep a solo rider's heading in sync in those cutscenes,
// since no such rider existed before this mod. Dual-ride cutscenes (Link riding, Zelda on the rear
// seat) are, per testing, already handled correctly by vanilla's own `shape_angle` logic in the
// large majority of cases -- that's the one scenario this engine code was actually written for and
// tested against. Overriding both seats indiscriminately was tried and regressed repeatedly
// (constant ~90 degree error substituting the root joint wholesale; sinking into Epona while
// rearing when deriving rotation from the saddle joint but leaving the rear seat's position as the
// original computed it; floating next to Epona when re-anchoring position to the root joint
// instead; an off-center seat entirely when deriving both position and rotation from the saddle
// joint together). All of those regressions occurred in the rear (dual-ride) seat specifically,
// which never needed fixing in the first place -- so the override below is scoped to the solo
// (front saddle) seat only, leaving the dual-ride seat's already-working vanilla computation
// completely untouched.
//
// For the solo seat, position is left exactly as the original, unmodified function already
// computed it (this hook runs *after* the original, so `current.pos` is already correct); only the
// drawn rotation is replaced, derived from `horse->getSaddleMtx()` (a live, fully up-to-date
// animated joint matrix that reacts to rearing, unlike the separate `shape_angle` field) via the
// same `mDoMtx_MtxToRot()` + `-0x4000` bind-pose correction technique `daAlink_c::setSyncHorsePos()`
// uses for Link's own rotation while riding.
//
// Scoped to our own spawned Zelda only: the vanilla duel's story-placed HoZelda already works
// correctly as-is, so it's left untouched to avoid any risk of regressing it.
DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);

static void on_hozelda_set_matrix_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!s_hasSpawnedZelda || (ActorId)fopAcM_GetID(zelda) != s_spawnedZeldaId) {
        return;
    }

    // Dual-ride (Link also on Epona) already works correctly via vanilla's own computation; only
    // the solo seat (no Link riding) needs the rotation override.
    if (daPy_getLinkPlayerActorClass()->checkHorseRide()) {
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || zelda->model == nullptr) {
        return;
    }

    csXyz rot;
    mDoMtx_MtxToRot(horse->getSaddleMtx(), &rot);
    rot.z += -0x4000;

    zelda->shape_angle = rot;
    zelda->current.angle.y = rot.y;

    mDoMtx_stack_c::transS(zelda->current.pos);
    mDoMtx_stack_c::ZXYrotM(rot.x, rot.y, rot.z);
    zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
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
    return MOD_OK;
}
}
