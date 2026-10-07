#include <cctype>
#include <cstdlib>
#include <cstring>

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

// Game includes
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_b_gnd.h"
#include "d/actor/d_a_e_sh.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/actor/d_a_player.h"
#include "d/d_cc_d.h"
#include "d/d_cc_uty.h"
#include "d/d_com_inf_game.h"
#include "d/d_event_manager.h"
#include "d/d_particle.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "helpers/string.hpp"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JParticle/JPAResourceManager.h"
#include "m_Do/m_Do_ext.h"
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
// just riding passively. Off by default ("Zelda active in combat" toggle). Exposed as a toggle in
// the mod's panel in the host Mods window for anyone who'd rather she actively fight.
static ConfigVarHandle g_cvarAutoTargetEnemies = 0;

static bool auto_target_enemies_enabled() {
    bool value = false;
    if (g_cvarAutoTargetEnemies == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarAutoTargetEnemies, &value) != MOD_OK) {
        return false;
    }
    return value;
}

// Whether `mod_update()` should log a snapshot of every signal that feeds into the cutscene
// hide/show decision (see `log_cutscene_diagnostics_if_changed()` below) any time one of them
// changes. Off by default: after several guess-based fixes for "Zelda stays hidden after certain
// cutscenes" each turned out to be wrong about what the engine was actually reporting at runtime,
// this toggle exists so a real playthrough can capture the actual values involved (event name,
// demo cut name, room/layer, etc.) instead of relying on reading the engine's source again.
// Exposed as a toggle in the mod's panel in the host Mods window, same as the others above.
static ConfigVarHandle g_cvarLogCutsceneDiagnostics = 0;

static bool log_cutscene_diagnostics_enabled() {
    bool value = false;
    if (g_cvarLogCutsceneDiagnostics == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarLogCutsceneDiagnostics, &value) != MOD_OK) {
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

// Whether `actor` is the HoZelda actor this mod spawned itself (as opposed to a story-placed one,
// e.g. the real Ganondorf duel's own Zelda), used throughout to scope hooks to our own actor only.
static bool is_spawned_zelda(const fopAc_ac_c* actor) {
    return s_hasSpawnedZelda && actor != nullptr &&
           (ActorId)fopAcM_GetID(actor) == s_spawnedZeldaId;
}

// Whether the mod's own spawned Zelda should currently be visually hidden (shrunk to an
// imperceptible size) rather than deleted, set once per tick by `mod_update()` and read back by
// the `HoZeldaSetMatrix` hook below, which does the actual hiding. See that hook's comment for why
// hiding her in place -- instead of deleting and later recreating the actor -- is necessary.
static bool s_hideSpawnedZeldaInCutscene = false;

// The opening title screen (Link/Epona galloping across Hyrule Field) is driven through the same
// `dScnPly_c` gameplay scene class as ordinary play, just requested under a different proc name
// (`fpcNm_OPENING_SCENE_e` instead of `fpcNm_PLAY_SCENE_e`, see `dComIfG_changeOpeningScene()` in
// `d_com_inf_game.cpp`), and that scene drives Epona exactly the same way a scripted cutscene
// does. Without specifically excluding it, that would make the title screen itself count as a
// "scripted cutscene" and hide this mod's Zelda there by default -- but she should always be
// visible on the title screen regardless of the cutscene-visibility toggle.
static bool is_title_screen() {
    scene_class* playScene = fopScnM_SearchByID(dStage_roomControl_c::getProcID());
    return playScene != nullptr && fpcM_GetName(playScene) == fpcNm_OPENING_SCENE_e;
}

// A handful of story cutscenes (currently "Demo01_01", "Demo01_02", "Demo36_00" and "Demo90_00")
// have their own staging/camera work built around Epona *not* visibly carrying a second rider,
// so this mod's own spawned Zelda must stay hidden through them no matter what the "Show Zelda
// during cutscenes" toggle is set to. Every other cutscene still respects that toggle normally.
//
// One earlier version of this check matched against `dStage_roomControl_c::getDemoArcName()`,
// the name of the currently-loaded cutscene demo *resource archive* -- but `loadDemoArchive()`
// only ever loads a new archive into an empty name slot as part of a *room* being created, and
// deliberately leaves it alone afterwards so any number of distinct cutscenes sharing that room
// can reuse its one cached archive without reloading it, so it never updates again once a later,
// different cutscene starts sharing that same room.
//
// A second version instead matched `dComIfGp_getEvent()->mEventId` (via `dEvDtEvent_c::getName()`
// off of `dEvent_manager_c::getEventData()`) -- `dEvt_control_c::mEventId` only gets assigned by
// that class's own `setParam()`, called from its `demoCheck()`/`talkCheck()`/etc. "order accepted"
// paths, so any cutscene instead ordered directly against `dEvent_manager_c::order()` (e.g. via a
// map-tool-triggered event, bypassing `dEvt_control_c` entirely) leaves `mEventId` stuck on
// whatever the last `dEvt_control_c`-routed event happened to be -- which is exactly backwards
// from a real signal and why that version hid the wrong cutscenes and showed the right one.
//
// `dEvent_manager_c` tracks the event it is *itself* actually running completely independently of
// `dEvt_control_c`, in its own `mCurrentEvId` member -- set by its own `order()` regardless of
// which caller invoked it -- and exposes it pre-built via `getRunEventName()` (named exactly for
// this purpose: it already returns "NO DATA"/"NOT RUNNING" sentinel strings, neither of which
// collide with any real cutscene name below, for the "nothing is running" and "between cuts"
// cases respectively instead of requiring extra null-checks here).
//
// A captured diagnostics log (see `log_cutscene_diagnostics_if_changed()`) from an actual
// playthrough of the Demo01 sequence showed `getRunEventName()` returning all-lowercase event
// names at runtime -- "demo01_01", "demo01_02", "demo01_03", and (for the title screen's own
// cutscene-like event) "demo38_01" -- while this list was written with the capitalized demo
// *archive*/file naming convention ("Demo01_01", "Demo01_02"). Because the comparison below used
// to be case-sensitive, none of these names ever actually matched anything, silently making this
// entire forced-hide list dead code; the comparison is now case-insensitive so it matches
// regardless of which convention a given event name happens to use.
//
// "Demo01_03" was briefly added here on the (incorrect) assumption that it was staged the same
// way as "Demo01_01"/"Demo01_02" just because it's part of the same numbered sequence sharing the
// same room -- but per direct confirmation, only "Demo01_01" and "Demo01_02" are actually staged
// without a second rider; "Demo01_03" should respect the "Show Zelda during cutscenes" toggle
// like any ordinary cutscene, so it does not belong in this forced-hide list.
static bool names_equal_case_insensitive(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool is_always_hidden_cutscene() {
    static const char* const kAlwaysHiddenDemoNames[] = {
        "Demo01_01",
        "Demo01_02",
        "Demo36_00",
        "Demo90_00",
    };
    const char* eventName = dComIfGp_getEventManager().getRunEventName();
    for (const char* name : kAlwaysHiddenDemoNames) {
        if (names_equal_case_insensitive(eventName, name)) {
            return true;
        }
    }
    return false;
}

// Snapshot of every signal `mod_update()` consults to decide whether this mod's own spawned
// Zelda should be hidden this tick. Kept as a plain struct (rather than just a block of local
// variables) so it can be compared whole against the previous tick's snapshot below.
struct CutsceneDiagnosticsSnapshot {
    bool horseDemoMode = false;
    bool titleScreen = false;
    bool storyHoZeldaActive = false;
    bool inScriptedCutscene = false;
    bool alwaysHiddenCutscene = false;
    bool showInCutscenesToggle = false;
    bool hideSpawnedZelda = false;
    int demoStaffId = -1;
    int roomNo = -1;
    int layerNo = -1;
    char eventName[64] = {0};
    char demoArcName[64] = {0};
    char cutName[64] = {0};

    bool operator==(const CutsceneDiagnosticsSnapshot& other) const {
        return horseDemoMode == other.horseDemoMode && titleScreen == other.titleScreen &&
               storyHoZeldaActive == other.storyHoZeldaActive &&
               inScriptedCutscene == other.inScriptedCutscene &&
               alwaysHiddenCutscene == other.alwaysHiddenCutscene &&
               showInCutscenesToggle == other.showInCutscenesToggle &&
               hideSpawnedZelda == other.hideSpawnedZelda && demoStaffId == other.demoStaffId &&
               roomNo == other.roomNo && layerNo == other.layerNo &&
               std::strcmp(eventName, other.eventName) == 0 &&
               std::strcmp(demoArcName, other.demoArcName) == 0 &&
               std::strcmp(cutName, other.cutName) == 0;
    }
    bool operator!=(const CutsceneDiagnosticsSnapshot& other) const { return !(*this == other); }
};

// Logs every signal feeding into the cutscene hide/show decision above, but only the instant any
// of them actually changes from the previous tick -- logging unconditionally every tick would
// flood the log with hundreds of identical lines per second during ordinary gameplay. Gated
// behind the "Log cutscene diagnostics" toggle (off by default) so it never runs unless someone
// is actively trying to capture real data for this bug, per `log_cutscene_diagnostics_enabled()`
// above.
static void log_cutscene_diagnostics_if_changed(daHorse_c* horse, bool titleScreen,
                                                 bool inScriptedCutscene,
                                                 bool storyHoZeldaActive) {
    if (!log_cutscene_diagnostics_enabled()) {
        return;
    }

    CutsceneDiagnosticsSnapshot snapshot;
    snapshot.horseDemoMode = horse->checkHorseDemoMode();
    snapshot.titleScreen = titleScreen;
    snapshot.storyHoZeldaActive = storyHoZeldaActive;
    snapshot.inScriptedCutscene = inScriptedCutscene;
    snapshot.alwaysHiddenCutscene = is_always_hidden_cutscene();
    snapshot.showInCutscenesToggle = show_zelda_in_cutscenes();
    snapshot.hideSpawnedZelda = s_hideSpawnedZeldaInCutscene;
    snapshot.demoStaffId = horse->m_demoStaffId;
    snapshot.roomNo = dComIfGp_roomControl_getStayNo();
    snapshot.layerNo = dComIfG_play_c::getLayerNo(0);
    SafeStringCopyTruncate(snapshot.eventName, dComIfGp_getEventManager().getRunEventName());
    SafeStringCopyTruncate(snapshot.demoArcName, dStage_roomControl_c::getDemoArcName());
    if (snapshot.demoStaffId >= 0) {
        char* cutName = dComIfGp_getEventManager().getMyNowCutNameStr(snapshot.demoStaffId);
        if (cutName != nullptr) {
            SafeStringCopyTruncate(snapshot.cutName, cutName);
        }
    }

    static CutsceneDiagnosticsSnapshot s_lastLoggedSnapshot;
    static bool s_hasLoggedOnce = false;
    if (s_hasLoggedOnce && snapshot == s_lastLoggedSnapshot) {
        return;
    }
    s_hasLoggedOnce = true;
    s_lastLoggedSnapshot = snapshot;

    mods::log::info(
        "cutscene diagnostics: event='{}' demoArc='{}' cutName='{}' demoStaffId={} room={} "
        "layer={} horseDemoMode={} titleScreen={} storyHoZeldaActive={} inScriptedCutscene={} "
        "alwaysHiddenCutscene={} showInCutscenesToggle={} hideSpawnedZelda={}",
        snapshot.eventName, snapshot.demoArcName, snapshot.cutName, snapshot.demoStaffId,
        snapshot.roomNo, snapshot.layerNo, snapshot.horseDemoMode, snapshot.titleScreen,
        snapshot.storyHoZeldaActive, snapshot.inScriptedCutscene, snapshot.alwaysHiddenCutscene,
        snapshot.showInCutscenesToggle, snapshot.hideSpawnedZelda);
}

// `fopAcIt_Judge()`'s filter for `find_other_hozelda()` below: matches any `HoZelda` actor other
// than our own spawned one.
static void* judge_other_hozelda(fopAc_ac_c* i_actor, void*) {
    if (fopAcM_GetName(i_actor) != fpcNm_HOZELDA_e || is_spawned_zelda(i_actor)) {
        return NULL;
    }
    return i_actor;
}

// Finds a `HoZelda` actor somewhere in the world that this mod did *not* spawn itself -- i.e. one
// placed by the story, such as the real horseback archery duel against Ganondorf (the only place
// in the vanilla game a `HoZelda` actor is ever placed directly rather than through this mod).
//
// Used to detect that real duel and bow out of its way entirely: this mod otherwise has no way to
// tell "the story is about to attach its own HoZelda to the horse" apart from the moment it
// actually happens, which can be one or more frames after this mod's own spawn/keep-mounted logic
// already ran for that same frame -- `d_a_horse.cpp`'s `execute()` only overwrites whichever
// actor is currently attached once the story's own HoZelda starts ticking, so there's otherwise a
// window where both our own spawned Zelda and the story's are simultaneously alive (the
// "duplicate Zelda" bug), and -- since our own hooks are scoped to our own actor via
// `is_spawned_zelda()` above -- our own Zelda would keep searching for and firing at nearby
// enemies the whole time, overlapping the real duel's own Ganondorf-only targeting. Checking for
// *any* other HoZelda in existence, rather than just whichever is currently attached to the horse,
// catches that window regardless of exactly which frame the handoff happens on.
static fopAc_ac_c* find_other_hozelda() {
    return (fopAc_ac_c*)fopAcIt_Judge((fopAcIt_JudgeFunc)judge_other_hozelda, nullptr);
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
// correctly as-is, so it's left untouched to avoid any risk of regressing it. The rear-blend fix
// above is additionally scoped to the solo seat only (no Link riding): the dual-ride (rear) seat
// already works correctly via vanilla's own computation outside of cutscenes and was never part of
// that particular clipping/floating bug.
//
// This same post-hook also fixes a separate rotation-mismatch bug during scripted cutscenes, for
// both the solo seat AND the dual-ride (Link also riding) seat: see the `inCutscene` branch further
// down in the hook's body for why a plain `shape_angle = horse->shape_angle` copy (vanilla's own,
// unconditional, already-correct technique for ordinary gameplay) isn't actually enough there, and
// what's done differently instead.
DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);

// How small a scale to shrink this mod's own spawned Zelda's rendered model to while she should
// be hidden during a scripted cutscene, small enough to be imperceptible to the player regardless
// of how close the cutscene's camera gets.
//
// This replaces an earlier approach of simply deleting (`fopAcM_delete`) the actor for the
// cutscene's duration and recreating it once the cutscene ended: `fopAcM_create` isn't safe to
// call while `daHorse_c` is being puppeted by scripted demo data (see `spawn_zelda_on_horse()`'s
// callers below), but more importantly, deleting *and recreating* her around every cutscene
// boundary was found to crash (SIGABRT) outright, almost certainly for the same underlying
// reason. Hiding her in place -- she's never deleted, just shrunk down where the player can't
// make her out -- sidesteps that entirely: the actor is always alive and ticking normally, so
// there's no unsafe create/delete anywhere near a cutscene boundary, and she reappears the instant
// the cutscene ends with no recreation (and no multi-frame delay) needed at all.
//
// An earlier version of this hiding also moved her model far underground, on top of shrinking it.
// That caused its own glitch: Epona's reins are attached to Zelda's hand whenever she's loaded and
// riding solo, so dragging her model's position itself down along with it dragged the reins
// straight down with her, visibly stretching them to that underground position every time she was
// hidden. Scale alone doesn't have that problem -- shrinking her model leaves her seated position
// (and thus the reins' attachment point) exactly where it already was, just imperceptibly small.
static const f32 kHiddenScale = 0.0001f;

static void hide_zelda_visually(daHoZelda_c* zelda) {
    mDoMtx_stack_c::transS(zelda->current.pos);
    mDoMtx_stack_c::ZXYrotM(zelda->shape_angle.x, zelda->shape_angle.y, zelda->shape_angle.z);
    mDoMtx_stack_c::scaleM(cXyz(kHiddenScale, kHiddenScale, kHiddenScale));
    zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
}

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

    // Hiding her for a cutscene overrides everything else below: her position/rotation/animation
    // don't matter while she's imperceptible underground, and `current.pos`/`shape_angle` (her
    // actual logical transform, read by other systems such as the horse-rider-flag clearing in
    // `mod_update()`) are deliberately left untouched -- only the matrix actually used to draw her
    // this frame is overridden, so she's back exactly where she belongs the instant hiding ends.
    if (s_hideSpawnedZeldaInCutscene) {
        if (zelda->model != nullptr) {
            hide_zelda_visually(zelda);
        }
        s_turnStandBlend = 0.0f;
        return;
    }

    // Dual-ride (Link also on Epona) is still in scope for the cutscene rotation fix further down
    // (see `inCutscene` below), just not for the rear-blend fix right above: that one addresses a
    // solo-seat-only clipping/floating bug that vanilla's own dual-ride computation never had.
    bool dualRide = daPy_getLinkPlayerActorClass()->checkHorseRide();

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || zelda->model == nullptr) {
        s_turnStandBlend = 0.0f;
        return;
    }

    // Eases toward 1 while rearing, back toward 0 once the rear ends; 1/8th per frame fully blends
    // in about 8 frames (roughly a tenth of a second at 60 FPS), fast enough to not be noticeable as
    // its own separate motion, but slow enough to erase the one-frame snap. Only chased while riding
    // solo -- see the comment on `dualRide` above.
    if (dualRide) {
        s_turnStandBlend = 0.0f;
    } else {
        cLib_chaseF(&s_turnStandBlend, horse->checkTurnStand() ? 1.0f : 0.0f, 1.0f / 8.0f);
    }

    // Whether a scripted cutscene (as opposed to ordinary player-driven riding) is currently
    // puppeting Epona.
    bool inCutscene = horse->checkHorseDemoMode();

    // Fully settled outside of a rear and outside of a cutscene: vanilla's own computation (already
    // run by the time this post-hook fires) is left completely untouched. For the dual-ride seat,
    // that's simply "outside of a cutscene" (the rear-blend fix never applies to it at all).
    if (!inCutscene && (dualRide || s_turnStandBlend <= 0.0f)) {
        return;
    }

    // Vanilla's own solo-seat local offset (`localFrontHorseRidePos` in `d_a_hozelda.cpp`), kept
    // here so it can be blended against during a rear (and reused as-is otherwise) -- outside of a
    // rear this is exactly what vanilla already uses, so blending starts and ends at the same place
    // vanilla would have placed her.
    static const Vec kZeldaFrontHorseRidePos = {-75.893997f, 57.61f, 4.079f};

    // `daAlink_c`'s own local ride offset for Link's solo riding position (`l_localHorseRidePos` in
    // `d_a_alink.cpp`, a file-local constant we can't reference directly). Blending toward it in
    // place of Zelda's own, further-forward/higher offset is the whole point of the rear fix: it's
    // the offset Link's own riding position already proves doesn't clip or float during a rear.
    static const Vec kLinkHorseRidePos = {-68.208984f, 41.609924f, 0.883789f};

    // Vanilla's own dual-ride (Link also riding) local offset (`localHorseRidePos` in
    // `d_a_hozelda.cpp`) -- the rear seat behind Link, used only to rebuild the cutscene-rotation
    // matrix below; never blended, since the rear-blend fix doesn't apply to this seat.
    static const Vec kZeldaDualRidePos = {-5.894f, 52.61f, 4.079f};

    Vec localPos = dualRide ? kZeldaDualRidePos : kZeldaFrontHorseRidePos;
    if (!dualRide && s_turnStandBlend > 0.0f) {
        localPos.x += (kLinkHorseRidePos.x - kZeldaFrontHorseRidePos.x) * s_turnStandBlend;
        localPos.y += (kLinkHorseRidePos.y - kZeldaFrontHorseRidePos.y) * s_turnStandBlend;
        localPos.z += (kLinkHorseRidePos.z - kZeldaFrontHorseRidePos.z) * s_turnStandBlend;

        mDoMtx_multVec(horse->getRootMtx(), &localPos, &zelda->current.pos);
    }

    // Kept in sync for other systems that read her logical transform (e.g. the horse-rider-flag
    // clearing and reins handling in `mod_update()`/`on_hozelda_execute_post`), even though the
    // drawn matrix itself is no longer reconstructed from these during a cutscene (see below).
    zelda->shape_angle = horse->shape_angle;
    zelda->current.angle.y = zelda->shape_angle.y;

    if (inCutscene) {
        // Several of Epona's scripted cutscene "tool demo" modes (`daHorse_c::procToolDemo()` in
        // `d_a_horse.cpp`) drive her purely through a baked animation root-motion track on her own
        // model's joint 0, *without* ever touching `shape_angle` at all -- the demo data's rotate
        // channel (`dDemo_actor_c::checkEnable(ENABLE_ROTATE_e)`) is frequently left unset for such
        // clips, relying entirely on the animation itself to turn her body. `horse->shape_angle`
        // then simply stays frozen at whatever heading she had before the cutscene started, even
        // while Epona's own drawn body visibly swivels via that animation -- exactly the "doesn't
        // face forward" bug, since a `shape_angle`-only copy (as used below for ordinary riding)
        // can't see or reproduce animation-only root motion that never touches `shape_angle` at
        // all.
        //
        // `horse->getRootMtx()` (her own model's joint 0, read fresh after Epona's own `calc()` for
        // this frame already ran) *does* reflect that root motion -- it's the actual matrix Epona
        // herself is drawn with. So instead of reconstructing Zelda's drawn matrix from the
        // `shape_angle` Euler angles (which may be stale), her whole base matrix is built directly
        // from that same joint matrix plus her local seat offset, rigidly gluing her orientation to
        // Epona's own drawn body for every cutscene frame, baked root motion or not. This mirrors
        // the technique already used for her *position* above (and vanilla's own, for ordinary
        // riding): a local offset carried through `horse->getRootMtx()`, just keeping the matrix's
        // rotation part intact instead of discarding it in favor of a separately-reconstructed one.
        //
        // Epona's joint-0 matrix itself isn't oriented the same way `shape_angle`'s Y axis treats
        // "facing forward" -- built straight from the model's own root joint, it's rotated a fixed
        // 90 degrees clockwise relative to that convention (Zelda's seat position above was already
        // correct through this same matrix, since translation isn't affected by that discrepancy,
        // but her orientation visibly came out facing off Epona's right side instead of forward).
        // Correcting for it here, as a local Y rotation applied *after* the translate above (so it
        // turns her in place around her already-correctly-placed seat position rather than moving
        // her), rotates her the needed 90 degrees counter-clockwise to align with Epona's actual
        // forward direction.
        mDoMtx_stack_c::copy(horse->getRootMtx());
        mDoMtx_stack_c::transM(localPos.x, localPos.y, localPos.z);
        mDoMtx_stack_c::YrotM(-0x4000);
        zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
    } else {
        mDoMtx_stack_c::transS(zelda->current.pos);
        mDoMtx_stack_c::ZXYrotM(zelda->shape_angle.x, zelda->shape_angle.y, zelda->shape_angle.z);
        zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
    }
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

// Whether `i_actor` is currently dormant/underground and therefore shouldn't be auto-targeted:
// vanilla itself treats these enemies as not really "present" while in this state (either
// collision-wise, status-wise, or both), so Light Arrows can't meaningfully hit them anyway.
//
//  - Stalhounds (`E_sh`/`e_sh_class`) spend most of the day buried underground, only surfacing for
//    a few in-game hours at night (`e_sh_stop()`'s own `hourOfDay` check in
//    `src/d/actor/d_a_e_sh.cpp`). Its outer state dispatcher (`action()`) only turns on normal
//    attention/targeting (`fopAcM_OnStatus`/`fopAc_AttnFlag_BATTLE_e`) for the above-ground states
//    (appear/move/attack/damage, `field_0x676` 1-3 and 10); the underground "stop" state
//    (`field_0x676 == 0`) and the sink-back-down "disappear" state (`field_0x676 == 5`) both leave
//    it off instead, exactly like a defeated one would be.
//
// Deku Babas and Baba Serpents are *not* handled here: unlike Stalhounds, their true
// intangibility window doesn't line up cleanly with a single state field (it actually extends a
// short while past the state transition that leaves "dormant", e.g. Deku Baba's
// `invulnerabilityTimer` and Baba Serpent's `field_0x69c[3]` both keep their hit/attack collision
// spheres shoved away for several frames after `action`/`field_0x66e` already reports them as no
// longer stay/dormant). Rather than keep chasing that exact window, the whole "Baba" family of
// enemies is instead excluded from auto-targeting outright in `judge_nearest_enemy()` below,
// dormant or not -- see the comment there for which actors that covers and why.
static bool is_dormant_enemy(fopAc_ac_c* i_actor) {
    switch (fopAcM_GetName(i_actor)) {
    case fpcNm_E_SH_e: {
        e_sh_class* stalhound = reinterpret_cast<e_sh_class*>(i_actor);
        return stalhound->field_0x676 == 0 || stalhound->field_0x676 == 5;
    }
    default:
        return false;
    }
}

// `fopAcIt_Judge()` stops and returns at the first non-NULL result, so to find the *nearest*
// enemy (rather than just the first one in the actor list) this always returns NULL -- keeping
// the iteration going over every actor -- and instead threads the closest candidate so far through
// `i_data`, read back once `fopAcIt_Judge()` itself returns.
static void* judge_nearest_enemy(fopAc_ac_c* i_actor, void* i_data) {
    HoZeldaTargetSearch* search = static_cast<HoZeldaTargetSearch*>(i_data);

    if (i_actor == search->self || fopAcM_GetGroup(i_actor) != fopAc_ENEMY_e ||
        fopAcM_GetName(i_actor) == fpcNm_E_WB_e || fopAcM_GetName(i_actor) == fpcNm_E_DB_e ||
        fopAcM_GetName(i_actor) == fpcNm_E_YD_e || fopAcM_GetName(i_actor) == fpcNm_E_HB_e ||
        fopAcM_GetName(i_actor) == fpcNm_E_YH_e || fopAcM_GetName(i_actor) == fpcNm_E_GB_e ||
        i_actor->health <= 0 || is_dormant_enemy(i_actor))
    {
        // No explicit Ganondorf (`fpcNm_B_GND_e`) exclusion is needed here: `B_gnd` is only ever
        // placed directly by the stage itself, in the one room that hosts the real horseback
        // archery duel -- and `mod_update()`'s `storyHoZeldaActive` check already tears down (and
        // withholds respawning) this mod's own Zelda for as long as that duel's own story-placed
        // HoZelda exists, so `judge_nearest_enemy()` never runs at all while a `B_gnd` actor could
        // possibly be alive to be found here.
        //
        // E_WB (Bullbo, the wild boar Bulblins ride) is excluded too: it's its own `fopAc_ENEMY_e`
        // actor separate from its Bulblin rider, but it never attacks on its own -- only the rider
        // does -- so for this mod's purposes it isn't a hostile target.
        //
        // The entire "Baba" family is excluded outright, dormant or not: E_DB (Deku Baba), E_YD
        // (Twilight Deku Baba), E_HB (Hebi Baba, i.e. Baba Serpent), E_YH (Twilight Hebi Baba) and
        // E_GB (Giant Baba). An earlier version of this exclusion list used `fpcNm_E_YD_e` under
        // the mistaken belief it was Baba Serpent -- per `d_stage.cpp`'s own stage object name
        // table and these actors' class doc comments, `E_yd`/`e_yd_class` is actually "Twilight
        // Deku Baba", while the real Baba Serpent is `E_hb`/`e_hb_class` (with `E_yh`/`e_yh_class`
        // as its own Twilight counterpart) -- so the real Baba Serpent was never actually excluded
        // until now. All five share the same Deku-Baba-like retract/intangible-while-dormant
        // design (see `is_dormant_enemy()`'s comment above), so none of them are worth chasing
        // precise per-state timing for: Zelda simply never auto-targets any of them, dormant or
        // not -- she can still hit them incidentally if the player leads her into melee range,
        // same as before this mod existed.
        //
        // Dormant/underground enemies (see `is_dormant_enemy()` above) are excluded too: Zelda
        // would otherwise auto-target Stalhounds that haven't surfaced for the night yet.
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

    // While a scripted cutscene is puppeting Epona (`checkHorseDemoMode()`, the same check
    // `mod_update()` uses to decide whether to hide this mod's own Zelda at all -- see
    // `inScriptedCutscene` there), she shouldn't keep drawing her bow and firing Light Arrows at
    // anything, whether she's currently visible or hidden (shrunk to an imperceptible size) for
    // that same cutscene. Visible, her firing arrows mid-cutscene would be a jarring, obviously
    // unscripted interruption the player never asked for; hidden, the player can't even see her
    // doing it, but nearby enemies (and NPCs, e.g. during an otherwise unrelated conversation with
    // Midna) could still visibly react to arrows seemingly coming from nowhere. Both cases are
    // treated exactly like the toggle being off: drop whatever target we'd previously picked so the
    // function below naturally falls back to doing nothing, the same as if she'd never found
    // anyone.
    //
    // The opening title screen is deliberately excluded from this check (`!is_title_screen()`,
    // mirroring `mod_update()`'s own exclusion): it drives Epona through the exact same
    // `checkHorseDemoMode()` machinery as a real cutscene, but this mod's Zelda is always shown
    // there regardless of the "Show Zelda during cutscenes" toggle, so there's no reason to
    // suppress her combat behavior there either.
    daHorse_c* horse = dComIfGp_getHorseActor();
    bool inCutscene = horse != nullptr && horse->checkHorseDemoMode() && !is_title_screen();

    if (auto_target_enemies_enabled() && !inCutscene) {
        zelda->mGndAcKeep.setData(find_nearest_enemy(zelda));
    } else {
        zelda->mGndAcKeep.clearData();
    }
    return HOOK_CONTINUE;
}

// Post-hook: while this mod's own spawned Zelda is hidden for a cutscene (`setMatrix()`, already
// run earlier this same `execute()` via the hook above, shrinks her model down to
// `kHiddenScale`), `daHoZelda_c::execute()` itself still unconditionally ends with
// `horse->setReinPosHand(6)` whenever she's riding solo (`mIsSingleRide`) and Link isn't mounted
// -- attaching Epona's reins to her hand joint (`getRightFingerMtx()`) regardless of whether she's
// currently visible. Shrinking her model collapses that joint down toward her own seated position
// rather than moving it away to nothing, so the reins end up visibly anchored at (approximately)
// where she's sitting instead of following her actual hand -- exactly as conspicuous as not hiding
// the reins at all.
//
// Once `execute()` returns, if she's currently hidden, this re-derives the reins' position from
// `daHorse_c::setReinPosNormal()` instead -- the same saddle-anchored position (`m_model`'s own
// joint 0x15) the reins already default to whenever nobody is riding solo at all. That function
// itself skips doing anything when `getZeldaActor()` is attached and riding solo (deferring to the
// hand-based logic instead, by design): since our Zelda is still attached at this point (`execute()`
// just reattached her, as it does every tick regardless of hiding), she's briefly detached
// (`setZeldaActor(nullptr)`) around this one call so `setReinPosNormal()` actually recomputes the
// reins. This has no lasting effect -- `execute()` unconditionally reattaches her
// (`horse->setZeldaActor(this)`) at the very start of next tick's call anyway, the same trick
// already used for the grass-whistle fix above.
static void on_hozelda_execute_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda) || !s_hideSpawnedZeldaInCutscene) {
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr) {
        return;
    }

    fopAc_ac_c* currentZelda = horse->getZeldaActor();
    if (currentZelda != nullptr && (ActorId)fopAcM_GetID(currentZelda) == s_spawnedZeldaId) {
        horse->setZeldaActor(nullptr);
    }
    horse->setReinPosNormal();
}

// Vanilla gates each Light Arrow shot behind two scripted 30-frame (0.5s) pauses in the bow
// draw/ready/shoot/recover state machine below: one between nocking the arrow and it becoming
// ready to fire, and one between a shot landing and the next draw starting. Shortening both
// speeds up our spawned Zelda's rate of fire, per the mod's design goal, without touching the
// draw/shoot animation clips themselves. Set much lower than half (30 -> 5) for testing whether a
// more aggressive cut is actually noticeable in-game.
static constexpr u8 kArrowWaitFrames = 5;

// With the scripted wait above shortened, the body/bow "ARELORDH"/"BARELORDH" draw animation
// (`mUpperAnmID == 8`, entered via `setUpperAnime(8)` + `setBowBck(0xB)` below) became the new
// bottleneck: vanilla always plays it back at its authored speed (`J3DFrameCtrl` rate 1.0, hardcoded
// inside `daHoZelda_c::setUpperAnime()`/`setBowBck()`) and only advances to the next state once it
// finishes (`mFrameCtrl[2].checkAnmEnd()`). Doubling both the body clip's frame-controller rate and
// the bow model's own matching draw clip rate halves the time that state takes, keeping the two in
// sync with each other.
static constexpr f32 kDrawAnimSpeedMultiplier = 2.0f;

// Replace hook for change (2) above: a copy of vanilla's `daHoZelda_c::setAnm()` (dusklight's
// `src/d/actor/d_a_hozelda.cpp`) for our own spawned Zelda only, with just the Ganondorf-specific
// targeting checks replaced (search the body for "deviates from vanilla" below), plus the
// faster-rate-of-fire change documented above (search for "kArrowWaitFrames"); every other branch
// is unmodified. Operates on `zelda->member` throughout, rather than `this->member`, since it
// isn't itself a member function.
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
    // case in practice: this mod's own Zelda is never alive at the same time as a `B_gnd` actor,
    // see `judge_nearest_enemy()`'s comment above), kept so his own mount/vulnerability checks
    // below still apply correctly rather than silently skipping them if it ever does happen.
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
                zelda->mAnmTimer = kArrowWaitFrames;
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
                zelda->mAnmTimer = kArrowWaitFrames;
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

            // Speed up the draw animation itself (see `kDrawAnimSpeedMultiplier` above): both the
            // body's upper-anim frame controller and the bow model's own draw clip need the same
            // rate bump, or the bow's string-pull would keep playing at normal speed after the
            // body's arms already finished drawing.
            if (anm_idx[2] == 8) {
                zelda->mFrameCtrl[2].setRate(kDrawAnimSpeedMultiplier);
                zelda->mBowBck.setPlaySpeed(kDrawAnimSpeedMultiplier);
            }
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

// --- One-hit-kill Light Arrows ---------------------------------------------------------------
//
// Vanilla's own `cc_at_check()` (`d_cc_uty.cpp`) already forces a flat 100 attack power for any
// hit whose collider material is `dCcD_MTRL_LIGHT` (i.e. any Light Arrow) against any ordinary
// enemy -- it only leaves Ganondorf (whose real duel damage/stagger is handled entirely
// separately) and Zant (deliberately left on normal, lower arrow damage) on their own un-boosted
// power. But a flat 100 power on its own still isn't a guaranteed kill: plenty of enemies simply
// start with more than 100 health, so they still take several Light Arrow hits in a row even
// though every single one of those hits already gets that same forced 100-power override.
//
// Rather than trying to special-case (or guess at) every individual enemy's own health-handling
// quirks -- several reuse their `health` field for non-damage bookkeeping elsewhere in their own
// state machines, so blindly zeroing it unconditionally would risk stepping on that -- this
// leaves vanilla's own forced-100-power subtraction completely unmodified and only clamps the
// *target's current health* down to within that guaranteed-lethal range immediately before the
// original runs. It's scoped with the exact same Light Arrow material check (and Ganondorf/Zant
// exemption) the original function itself applies a few lines later, so this never fires for a
// hit that wouldn't already be getting vanilla's own 100-power override anyway, and never touches
// health on any frame that isn't a Light Arrow hit at all.
//
// King Bulblin (`e_rdb_class`, `src/d/actor/d_a_e_rdb.cpp`'s `daE_RDB_Create()`, 600-900 health)
// is additionally exempted here, alongside Ganondorf and Zant: he's a dedicated sub-boss fight
// (the joust/one-on-one battles), and one-shotting him with a single Light Arrow would trivialize
// that encounter. Every other enemy -- including ordinary Bulblin riders (`e_rd_class`, 40
// health, already a one-shot even without this hook) -- still gets the full one-hit kill.
DEFINE_HOOK(&cc_at_check, CcAtCheck);

static HookAction on_cc_at_check_pre(ModContext*, void* args, void*, void*) {
    fopAc_ac_c* enemy = mods::arg<fopAc_ac_c*>(args, 0);
    dCcU_AtInfo* atInfo = mods::arg<dCcU_AtInfo*>(args, 1);

    if (enemy == nullptr || atInfo == nullptr || atInfo->mpCollider == nullptr) {
        return HOOK_CONTINUE;
    }

    if (static_cast<dCcD_GObjInf*>(atInfo->mpCollider)->GetAtMtrl() == dCcD_MTRL_LIGHT) {
        s16 name = fopAcM_GetName(enemy);
        if (name != fpcNm_B_GND_e && name != fpcNm_B_ZANT_e && name != fpcNm_E_RDB_e &&
            enemy->health > 100) {
            enemy->health = 100;
        }
    }

    return HOOK_CONTINUE;
}

// Zelda's Light Arrow trail/hit mark/charge effects (`daArrow_c::setBlur`/`setLightArrowHitMark`/
// `setLightChargeEffect`, particle IDs 0x896E-0x8978) -- plus a number of other effects that turned
// out to live in the very same archive once this was tested in-game (e.g. whatever produces the
// arrow's actual visible light trail, which isn't any of those eleven IDs) -- only exist in the
// horseback-duel stage's room-specific particle archive (`/res/Particle/Pscene181.jpc`) today, not
// in the always-resident common bank (`/res/Particle/common.jpc`), so they're invisible whenever
// this mod's spawned-in Zelda fires Light Arrows anywhere outside that one vanilla cutscene. Since
// the mod can't ship a modified `common.jpc` (that would mean redistributing edited copyrighted
// game assets), the fix instead teaches the engine to keep `Pscene181.jpc` loaded as a *third*,
// always-resident particle bank -- alongside `common.jpc` (bank 0) and whatever per-room
// `Pscene###.jpc` is normally loaded (bank 1) -- and redirects every particle ID that archive
// actually contains to it. Rather than maintaining a hand-picked allowlist of IDs (which turned out
// to be incomplete and a maintenance burden -- Zelda's effects aren't the only things in that
// archive, but anything else in it is simply never requested outside the real duel, so redirecting
// unconditionally is harmless), `getRM_ID()` below queries bank 2's own `JPAResourceManager`
// directly via `JPAResourceManager::getResource()`.
//
// `dPa_control_c::mEmitterMng` (a `JPAEmitterManager`) only ever provisions 2 resource-manager
// slots (`ridMax`, hardcoded at construction in `dPa_control_c::createCommon()`), so bank 2 has to
// be made to exist first:
//   1. Grow `ridMax` from 2 to 3 right after `mEmitterMng` is constructed, by hooking
//      `JPAEmitterManager::entryResourceManager()` (a normal, addressable member function) and
//      reallocating `pResMgrAry` with room for bank 2 the moment that method is first called
//      (always with `resMgrID == 0`, registering bank 0, immediately after construction in
//      `createCommon()`). An earlier attempt hooked the constructor itself instead, by symbol
//      name (constructors have no pointer-to-member-function form in C++); that only ever worked
//      by accident; see the comment above `ParticleEntryResourceManager` below for why it was
//      replaced.
//   2. Once `createCommon()` finishes setting up bank 0, synchronously load `Pscene181.jpc`'s raw
//      bytes into a small dedicated heap of this mod's own (see `s_lightArrowHeap` below) --
//      *not* `dPa_control_c`'s own resident particle heap; see that variable's comment for why.
//   3. Build a `JPAResourceManager` over it and register it as bank 2, right there in the same
//      function -- see the comment above `on_particle_create_common_post` for why this load isn't
//      kicked off asynchronously the way vanilla's own per-room scene loads are.
//   4. Finally, `dPa_control_c::getRM_ID()` (which maps a particle ID to the bank it lives in) is
//      replaced so that, once bank 2 is ready, any particle ID that `Pscene181.jpc`'s own resource
//      manager actually contains resolves to it; everything else keeps using vanilla's existing
//      bank 0/1 logic unchanged.
static const u8 kLightArrowResMgrId = 2;

static bool s_lightArrowJpcReady = false;
static JPAResourceManager* s_lightArrowResMgr = nullptr;

// `Pscene181.jpc`'s raw archive bytes and its `JPAResourceManager` need to live *somewhere* for the
// entire game session, but NOT in `dPa_control_c`'s own resident particle heap (`m_resHeap`):
// that heap is itself a fixed-size `JKRExpHeap` carved out of `mDoExt_getArchiveHeap()`
// (`d_particle.cpp`: `JKRCreateExpHeap(heapSize, mDoExt_getArchiveHeap(), false)`), and
// `mDoExt_getArchiveHeap()` is a tightly budgeted heap shared with ordinary per-scene stage
// resource loading (room archives, camera data, event lists, and so on -- see
// `mDoExt_getSafeArchiveHeapSize()` and its users in `d_s_play.cpp`). A first attempt grew
// `m_resHeap` directly (by hooking `JKRExpHeap::create` and bumping the size of just that one
// allocation by a safety margin), which did make room for `Pscene181.jpc` itself, but permanently
// shrank `mDoExt_getArchiveHeap()`'s own remaining budget for everything else by that same
// margin -- every byte added here came out of stage loading's budget. That regressed into a
// `JKRExpHeap` allocation-failure `SIGABRT` while loading an unrelated new scene (crash log in
// `res/`), since that scene's stage resources no longer fit in the now-smaller remaining budget.
//
// Instead, this archive gets its own small, completely separate heap (`s_lightArrowHeap`). A
// second attempt carved that heap's backing memory out of `mDoExt_getZeldaHeap()` via the normal
// `JKRExpHeap::create(size, parent, errorFlag)` overload (the Zelda heap being the engine's own
// established "has the most slack" heap -- at boot it's sized to whatever memory is left over
// after every other heap has already claimed its fixed budget, and it's the heap vanilla's own
// `mDoDvdThd_mountArchive_c::execute` retries into as a last resort on every platform when every
// other heap fails to allocate). That still crashed, twice, for two different reasons:
//
//   1. On every build target this SDK supports other than the original consoles (`#if TARGET_PC`,
//      i.e. every platform this mod actually ships for), `JKRExpHeap::do_alloc()` treats *any*
//      allocation failure on *any* `JKRExpHeap` as unconditionally fatal and aborts the whole
//      process -- regardless of the `errorFlag` passed to that heap's own constructor (that flag
//      only matters on the non-PC path). That includes the implicit allocation
//      `JKRExpHeap::create(size, parent, errorFlag)` itself makes from its *parent* heap to back
//      the new heap's own memory: if the parent doesn't happen to have `kLightArrowHeapSize` free
//      at that exact moment, `create()` doesn't return `nullptr` the way its signature suggests;
//      it crashes immediately, inside the parent's own `do_alloc()`, before `create()` ever gets a
//      chance to return.
//   2. A `getMaxAllocatableSize()` pre-check (which cannot itself fail) was added to gate that
//      call and avoid attempting an allocation already known not to fit -- but the crash recurred
//      anyway, still during `dScnLogo_c`'s very first few frames before even the title screen,
//      with no trace of this mod's own diagnostic log lines (crash logs in `res/`). Whatever room
//      the pre-check measured evidently didn't hold by the time `create()` actually ran: other
//      mods load their own boot-time resources into shared heaps during this same narrow window,
//      and nothing stops something else from claiming that same space in between the check and
//      the allocation it was guarding.
//
// Chasing a *third* boot-time memory-pressure failure mode in the same shared heap wasn't worth
// it: this heap's backing memory now comes from `std::malloc()` instead, bypassing the JKR
// allocation system -- and every heap it manages -- entirely. `JKRExpHeap` has a second `create()`
// overload, `create(void* ptr, u32 size, JKRHeap* parent, bool errorFlag)`, that wraps
// caller-provided memory as a heap's storage directly, with no call to `JKRAllocFromHeap()` on
// `parent` at all (that parameter is used only to register this heap as a child in the engine's
// heap tree for bookkeeping/diagnostics, e.g. heap-dump tooling -- it's never range-checked against
// `ptr`, so `mDoExt_getZeldaHeap()` is passed here purely as a plausible place to be listed, not as
// a source of memory). `std::malloc()` is ordinary host-side allocation, entirely independent of
// every one of the game's own fixed-budget heaps (archive, game, Zelda, J2D, command): if the
// process is so low on memory that it fails, it simply returns `nullptr`, exactly like any other
// `std::malloc()` call elsewhere in this codebase, with no risk of the `CRASH()` this whole saga
// was trying to avoid. The one caveat: `JKRExpHeap::do_destroy()` only frees backing memory it
// allocated itself (the `size`-only `create()` overload); for this pointer-based overload, it just
// destructs in place and leaves the memory alone, so the raw backing pointer
// (`s_lightArrowHeapBacking` below) has to be freed with a matching `std::free()` call, by hand,
// after every `s_lightArrowHeap->destroy()`.
//
// The diagnostic free-size log below (`on_particle_create_common_post`) measured the archive +
// its resource manager using about 103 KB on the platforms this was tested on -- but the crash
// chain documented above `s_lightArrowHeap` kept recurring, in the same spot, even after every
// other heap this feature used to depend on (the shared archive heap, the Zelda heap, the DVD
// command heap) was removed from its load path one by one, on Windows specifically, while Android
// and Linux builds of the exact same code never showed any problem. Taken together with the fact
// that none of this function's own `mods::log::warn()` failure messages below have *ever* appeared
// in a single crash log across this entire saga, the likely explanation is that this 256 KiB budget
// itself was simply too tight on Windows (where identical source can still produce different struct
// layouts/alignment and therefore different real memory usage for the same archive, under a
// different compiler/ABI than the Linux and Android builds) -- and a too-tight `JKRExpHeap` doesn't
// *fail gracefully* the way this function's own `nullptr` checks assume: every allocation out of it
// (including the one inside `JKRDvdToMainRam()` and the one building the `JPAResourceManager`
// itself) still goes through the same unconditionally-fatal `JKRExpHeap::do_alloc()` described
// above, which aborts the whole process *before* ever returning `nullptr` to this code. In other
// words, this heap being our own and fully private only fixed *where* an allocation failure could
// no longer come from (every other heap in the game); it never made a failure *survivable* in the
// first place -- there's no such thing as "Light Arrow particles unavailable" if this heap itself
// runs out of room, only a hard crash.
//
// Since this heap's backing memory is ordinary host-process `std::malloc()`, entirely outside of
// and invisible to every one of the game's own fixed memory budgets, there is no longer any reason
// to keep it small the way the two heaps before it had to be (every byte here costs nothing to any
// other system, mod, or platform) -- so instead of continuing to guess at an exact byte count that
// happens to survive on every compiler/ABI this mod ships for, it's sized with a large, deliberately
// wasteful safety margin well beyond anything a ~200 KB archive plus its resource manager should
// plausibly need on any platform.
static const u32 kLightArrowHeapSize = 0x200000;  // 2 MiB for Pscene181.jpc and its resource
                                                   // manager, allocated from outside any JKR heap

static void* s_lightArrowHeapBacking = nullptr;
static JKRExpHeap* s_lightArrowHeap = nullptr;

// With `s_lightArrowHeap` itself no longer dependent on any shared heap having room, the crash
// still recurred a third time, in the same spot, right after this heap's creation stopped being
// able to fail that way -- pointing at the *next* thing this code did: kicking off the archive
// load via `mDoDvdThd_toMainRam_c::create()`, mirroring how vanilla's own `readScene()` loads every
// per-room scene archive. That call allocates its own small command object from
// `mDoExt_getCommandHeap()` (`m_Do_dvd_thread.cpp`) -- a single, tiny (a few KiB), engine-wide
// `JKRExpHeap` shared by *every* in-flight DVD read the whole game and every active mod issues,
// nowhere near as generously sized as the multi-megabyte heaps above. Vanilla itself relies on this
// same heap to load `common.jpc` at this exact boot moment (`d_s_logo.cpp`), so it isn't normally a
// bottleneck in a vanilla, single-mod-free boot -- but it's still a *shared* `JKRExpHeap`, subject
// to the exact same TARGET_PC unconditional-abort-on-failure behavior described above, and this
// mod's own additional load is one more simultaneous claim against it during the single busiest,
// most contested moment of the entire game session, where other active mods are independently
// loading their own boot-time resources too.
//
// Rather than adding a fourth heap-sizing workaround to a heap this mod doesn't own and can't
// resize, this load bypasses the asynchronous command-queue system entirely: `JKRDvdToMainRam()`
// (`JKRDvdRipper.h`) is the same low-level, synchronous DVD-read routine `mDoDvdThd_toMainRam_c`
// itself calls on the DVD thread, and it's already used directly, synchronously, by plenty of other
// vanilla code (`JKRDvdArchive`, `JKRCompArchive`, `JKRAramArchive` constructors) with no command
// object and no command heap involved at all -- it reads the file into a buffer it allocates
// directly from whatever heap is passed to it (`JKRAllocFromHeap(heap, ...)`), which here is
// `s_lightArrowHeap`: this mod's own private, `std::malloc`-backed heap, fully isolated from every
// other heap in the game and every other mod. Calling it directly, synchronously, right here in
// `on_particle_create_common_post` (rather than polling an async command every frame, the way
// `poll_light_arrow_particle_bank()` used to) means this entire feature's load path now has zero
// remaining dependency on any heap this mod doesn't fully own and control itself.

// Named hooks on `JPAEmitterManager`'s constructor turned out to be fragile across platforms:
// constructors have no pointer-to-member-function form in C++, so the only way to target one
// via `DEFINE_HOOK_SYMBOL` is by name. A first attempt hardcoded the Itanium-mangled symbol
// (`_ZN17JPAEmitterManagerC1EjjP7JKRHeaphh`), which only exists in the symbol manifest for build
// targets using the Itanium ABI and failed to resolve on Windows x86_64 ("symbol ... not found").
// Switching to the unmangled qualified display name (`"JPAEmitterManager::JPAEmitterManager"`)
// fixed that, but traded it for two worse problems: it failed to *build* on AppleClang arm64
// (macOS and iOS), and crashed on *initial load* on Windows x86_64 -- a named hook on a
// constructor is simply not a portable, well-supported pattern across the SDK's compilers.
//
// Instead, this hooks `JPAEmitterManager::entryResourceManager()` -- an ordinary non-virtual
// member function, addressable the normal way with `&Class::method`, no name resolution involved
// at all. `dPa_control_c::createCommon()` always calls it immediately after constructing
// `mEmitterMng`, registering bank 0 with `resMgrID == 0` -- the first and only call made with
// that ID. At that exact moment `pResMgrAry` already exists (built by the constructor with its
// original, too-small `ridMax`) but hasn't been touched yet, and -- critically -- the solid heap
// it was allocated from (`dPa_control_c::mHeap`) hasn't been trimmed to size yet either: that
// only happens via `mDoExt_adjustSolidHeap()` at the very end of `createCommon()`, well after
// every `entryResourceManager()` call it makes. So there's still room to grow `pResMgrAry` here,
// exactly as if the constructor itself had been asked to allocate a bigger one.
DEFINE_HOOK(&JPAEmitterManager::entryResourceManager, ParticleEntryResourceManager);

static HookAction on_particle_entry_resource_manager_pre(ModContext*, void* args, void*, void*) {
    auto* mgr = mods::arg<JPAEmitterManager*>(args, 0);
    u8 resMgrId = mods::arg<u8>(args, 2);
    if (resMgrId != 0 || mgr->ridMax > kLightArrowResMgrId) {
        return HOOK_CONTINUE;
    }

    u8 newRidMax = kLightArrowResMgrId + 1;
    JKRHeap* heap = g_dComIfG_gameInfo.play.getParticle()->getHeap();
    auto* newAry = JKR_NEW_ARRAY_ARGS(JPAResourceManager*, newRidMax, heap, 0);
    if (newAry == nullptr) {
        return HOOK_CONTINUE;
    }

    for (u8 i = 0; i < newRidMax; i++) {
        newAry[i] = (i < mgr->ridMax) ? mgr->pResMgrAry[i] : nullptr;
    }
    mgr->pResMgrAry = newAry;
    mgr->ridMax = newRidMax;
    return HOOK_CONTINUE;
}

// Loads `Pscene181.jpc`'s raw bytes right after vanilla finishes setting up the common bank (and,
// critically, after `mEmitterMng` -- constructed earlier in this same function -- has already had
// its slot count bumped by the hook above), then immediately builds a `JPAResourceManager` over it
// and registers it as bank 2, all synchronously, right here, rather than kicking off an
// asynchronous load and polling for it to finish on a later frame (see the comment above this load
// for why).
//
// `createCommon()` is NOT a true one-shot, console-boot-only call: it's only ever invoked from one
// call site (`dScnLogo_c`'s boot-time phase), but Twilight Princess implements several "big" scene
// transitions (dying and choosing to continue, certain major stage-to-stage crossings) as a full
// software reset (`mDoRst`) that tears down and rebuilds the entire particle system from scratch --
// a brand new `dPa_control_c`, a brand new resident heap, and a brand new `JPAEmitterManager` with
// an empty bank 2 slot -- which re-enters this very function. The mod's own static state below
// lives outside that reset, so it must be unconditionally re-armed here every time this runs,
// rather than treated as a permanent latch; otherwise, after the first such reset, `s_lightArrowJpcReady`
// stays stuck `true` forever even though the new `JPAEmitterManager` never actually got bank 2
// registered, silently breaking every Light Arrow hit mark/charge effect from that point on.
DEFINE_HOOK(&dPa_control_c::createCommon, ParticleCreateCommon);

static void on_particle_create_common_post(ModContext*, void*, void*, void*) {
    s_lightArrowJpcReady = false;
    // The previous resource manager (if any) belonged to the heap being destroyed/recreated
    // below, so this pointer must not be dereferenced until a new one is built.
    s_lightArrowResMgr = nullptr;

    // `s_lightArrowHeap` is this mod's own heap (see its declaration above), independent of
    // `dPa_control_c`'s lifecycle, but its *contents* (the previous load's archive bytes and
    // resource manager, if any) belong to a bank mapping that's no longer valid once this runs
    // again -- so it's destroyed and recreated fresh here, exactly like vanilla's own resident
    // particle heap is, rather than reused in place.
    if (s_lightArrowHeap != nullptr) {
        s_lightArrowHeap->destroy();
        s_lightArrowHeap = nullptr;
    }
    // The pointer-based `JKRExpHeap::create()` overload used below doesn't free this itself (see
    // the comment above this function) -- it has to be freed by hand, every time, before the next
    // `std::malloc()` call replaces it.
    if (s_lightArrowHeapBacking != nullptr) {
        std::free(s_lightArrowHeapBacking);
        s_lightArrowHeapBacking = nullptr;
    }

    // Ordinary host-side allocation, entirely outside the JKR heap system: on failure this simply
    // returns `nullptr`, with none of the abort-on-failure behavior described above this function.
    s_lightArrowHeapBacking = std::malloc(kLightArrowHeapSize);
    if (s_lightArrowHeapBacking == nullptr) {
        mods::log::warn("failed to allocate memory for Pscene181.jpc, Light Arrow particles "
                         "outside the horseback duel will be unavailable");
        return;
    }

    s_lightArrowHeap = JKRExpHeap::create(s_lightArrowHeapBacking, kLightArrowHeapSize,
                                           mDoExt_getZeldaHeap(), false);
    if (s_lightArrowHeap == nullptr) {
        std::free(s_lightArrowHeapBacking);
        s_lightArrowHeapBacking = nullptr;
        mods::log::warn("failed to create heap for Pscene181.jpc, Light Arrow particles outside "
                         "the horseback duel will be unavailable");
        return;
    }

    // Synchronous, direct DVD read: the same low-level routine `mDoDvdThd_toMainRam_c` itself
    // calls on the DVD thread, but invoked here directly, with no command object and no command
    // heap involved at all (see the comment above `s_lightArrowHeap` for why this replaced the
    // async `mDoDvdThd_toMainRam_c::create()` approach used previously). The destination buffer is
    // allocated straight out of `s_lightArrowHeap` -- this mod's own private heap -- with no other
    // heap touched anywhere in this call.
    u32 jpcSize = 0;
    void* jpcData = JKRDvdToMainRam("/res/Particle/Pscene181.jpc", nullptr, EXPAND_SWITCH_UNKNOWN1,
                                     0, s_lightArrowHeap, JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0,
                                     nullptr, &jpcSize);
    if (jpcData == nullptr) {
        mods::log::warn("Pscene181.jpc failed to load, Light Arrow particles outside the "
                         "horseback duel will be unavailable");
        return;
    }

    JKRHeap* heap = s_lightArrowHeap;
    JPAResourceManager* mgr = JKR_NEW_ARGS(heap, 0) JPAResourceManager(jpcData, heap);
    if (mgr == nullptr) {
        mods::log::warn("failed to build Pscene181.jpc resource manager, Light Arrow particles "
                         "outside the horseback duel will be unavailable");
        return;
    }

    dPa_control_c::getEmitterManager()->entryResourceManager(mgr, kLightArrowResMgrId);
    s_lightArrowResMgr = mgr;
    s_lightArrowJpcReady = true;

    // Logged once per load (including after every soft-reset reload) so this can be checked
    // against `kLightArrowHeapSize` above without needing a debugger: if this ever trends
    // towards 0, the heap needs to grow again, the same way it did when it was first sized.
    mods::log::info("Pscene181.jpc loaded as particle bank {}, resource heap has {} bytes free",
                     kLightArrowResMgrId, heap->getFreeSize());
}

// Once bank 2 is ready, redirects any particle ID that `Pscene181.jpc`'s own resource manager
// actually contains to it; everything else (and these same IDs, while bank 2 is still loading)
// keeps using vanilla's existing top-bit common/scene selection unchanged.
DEFINE_HOOK(&dPa_control_c::getRM_ID, ParticleGetRmId);

static void on_particle_get_rm_id_replace(ModContext*, void* args, void* retval, void*) {
    u16 nameId = mods::arg<u16>(args, 0);
    bool isPscene181Particle =
        s_lightArrowResMgr != nullptr && s_lightArrowResMgr->getResource(nameId) != nullptr;
    u8 result = (s_lightArrowJpcReady && isPscene181Particle)
                    ? kLightArrowResMgrId
                    : ParticleGetRmId::g_orig(nameId);
    if (retval != nullptr) {
        *static_cast<u8*>(retval) = result;
    }
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
                        "their own story-placed Zelda, the horse call/grass whistle, the opening "
                        "title screen, and area/scene transitions are unaffected either way -- "
                        "she always stays visible there.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvarShowInCutscenes;
    svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);

    UiControlDesc autoTargetControl = UI_CONTROL_DESC_INIT;
    autoTargetControl.kind = UI_CONTROL_TOGGLE;
    autoTargetControl.label = "Zelda active in combat";
    autoTargetControl.help_rml = "When on, Zelda automatically draws her bow and fires Light "
                                  "Arrows at nearby enemies, the same way she already does at "
                                  "Ganondorf during the horseback duel. When off (default), she "
                                  "just rides along passively.";
    autoTargetControl.binding = UI_BINDING_CONFIG_VAR;
    autoTargetControl.config_var = g_cvarAutoTargetEnemies;
    svc_ui->pane_add_control(mod_ctx, panel, &autoTargetControl, nullptr);

    UiControlDesc diagnosticsControl = UI_CONTROL_DESC_INIT;
    diagnosticsControl.kind = UI_CONTROL_TOGGLE;
    diagnosticsControl.label = "Log cutscene diagnostics";
    diagnosticsControl.help_rml = "When on, logs the event name, cutscene name, room/layer, and "
                                   "every other signal this mod uses to decide whether to hide "
                                   "Zelda during a cutscene, any time one of them changes. Off "
                                   "by default -- only useful for reporting bugs with the "
                                   "cutscene hide/show behavior above.";
    diagnosticsControl.binding = UI_BINDING_CONFIG_VAR;
    diagnosticsControl.config_var = g_cvarLogCutsceneDiagnostics;
    svc_ui->pane_add_control(mod_ctx, panel, &diagnosticsControl, nullptr);
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

    result = mods::hook::add_post<HoZeldaExecute>(on_hozelda_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda execute post, Epona's reins may follow her hand while she's "
            "hidden in cutscenes: {}",
            (int)result);
        // Not fatal: the mod still works, just without the hidden-reins fix.
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

    result = mods::hook::add_pre<CcAtCheck>(on_cc_at_check_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook attack-power check, Light Arrows won't one-hit-kill high-health "
            "enemies: {}",
            (int)result);
        // Not fatal: the mod still works, Light Arrows just keep vanilla's own flat-100-power
        // behavior (still enough to one-shot most ordinary enemies, just not high-health ones).
    }

    result = mods::hook::add_pre<ParticleEntryResourceManager>(on_particle_entry_resource_manager_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook particle emitter manager resource registration, Light Arrow particles "
            "outside the horseback duel will be unavailable: {}",
            (int)result);
        // Not fatal: the mod still works, Light Arrows just won't show their hit mark/charge
        // effects outside the one vanilla cutscene that already has them.
    }

    result = mods::hook::add_post<ParticleCreateCommon>(on_particle_create_common_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook particle common bank creation, Light Arrow particles outside the "
            "horseback duel will be unavailable: {}",
            (int)result);
        // Not fatal: same as above.
    }

    result = mods::hook::replace<ParticleGetRmId>(on_particle_get_rm_id_replace);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook particle resource manager selection, Light Arrow particles outside "
            "the horseback duel will be unavailable: {}",
            (int)result);
        // Not fatal: same as above.
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
    autoTargetCvarDesc.default_bool = false;
    result = svc_config->register_var(mod_ctx, &autoTargetCvarDesc, &g_cvarAutoTargetEnemies);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to register autoTargetEnemies option, defaulting to auto-targeting off: {}",
            (int)result);
        // Not fatal: `auto_target_enemies_enabled()` already falls back to "off" when the var
        // isn't registered.
    }

    ConfigVarDesc diagnosticsCvarDesc = CONFIG_VAR_DESC_INIT;
    diagnosticsCvarDesc.name = "logCutsceneDiagnostics";
    diagnosticsCvarDesc.type = CONFIG_VAR_BOOL;
    diagnosticsCvarDesc.default_bool = false;
    result = svc_config->register_var(mod_ctx, &diagnosticsCvarDesc, &g_cvarLogCutsceneDiagnostics);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to register logCutsceneDiagnostics option, diagnostics logging will stay "
            "off: {}",
            (int)result);
        // Not fatal: `log_cutscene_diagnostics_enabled()` already falls back to "off" when the
        // var isn't registered.
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
        s_hideSpawnedZeldaInCutscene = false;
        return MOD_OK;
    }

    if (horse->checkHorseCallWait()) {
        // Right after loading a save made in a different area than Epona was left in,
        // `daHorse_c::create()` sets `FLG0_NO_DRAW_WAIT` on her instead of restoring her saved
        // position (see that function's own stage/room-name comparison against
        // `dComIfGs_getHorseRestartStageName()`/`...RoomNo()`), and both `draw()` and `execute()`
        // return immediately while it's set -- vanilla Epona is loaded but neither drawn nor
        // given a chance to update her physics/collision. She stays that way until the player
        // calls her with the grass whistle (`callHorseSubstance()` clears the flag the moment
        // it's set). Without this check, this mod would still spawn/keep its own Zelda riding a
        // horse that is itself invisible and intangible, leaving Zelda floating there on her own
        // with no visible Epona underneath her. Treat this exactly like the "no horse loaded"
        // case above.
        remove_spawned_zelda();
        s_hideSpawnedZeldaInCutscene = false;
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
    // driving Epona. `daHorse_c::checkHorseDemoMode()` (`field_0x16b8 != 0`) is set by
    // `setDemoData()` any time the horse is being driven by something other than the player --
    // i.e. a scripted cutscene -- including ordinary non-"tool demo" procs (jump/stop/turn/move/
    // wait) that a cutscene can puppet her through, not just the single `procToolDemo()`
    // (`PROC_TOOL_DEMO_e`) state. An earlier version of this check only looked at
    // `PROC_TOOL_DEMO_e`, which missed every cutscene that drives Epona via one of those other
    // procs instead, letting this mod's own Zelda incorrectly appear in cutscenes she shouldn't
    // regardless of the "Show Zelda during cutscenes" toggle.
    //
    // The only case that should always stay visible regardless of that toggle is the opening
    // title screen (`is_title_screen()`, which drives Epona the exact same way a cutscene does --
    // see that function's own comment). Every other `checkHorseDemoMode()` event -- including the
    // horse-call/grass-whistle gallop-back, NPC conversations, and area/scene transitions -- is
    // treated the same as any other story cutscene and hidden by default.
    bool inScriptedCutscene = horse->checkHorseDemoMode() && !is_title_screen();

    // Whether a story-placed HoZelda (never one this mod spawned) currently exists anywhere, e.g.
    // the real horseback archery duel against Ganondorf, where the game itself places and drives
    // its own HoZelda riding solo. This mod's own spawned Zelda would otherwise duplicate her, and
    // -- since this mod's auto-target/combat hooks are scoped to our own actor only -- our own
    // Zelda would keep auto-targeting nearby enemies the entire time, overlapping the real duel's
    // own Ganondorf-only targeting. So this mod's entire functionality simply steps aside for as
    // long as the real duel's own HoZelda exists: our own spawned Zelda is torn down and not
    // respawned until the story's HoZelda is gone again.
    bool storyHoZeldaActive = find_other_hozelda() != nullptr;

    if (storyHoZeldaActive) {
        remove_spawned_zelda();
        s_hideSpawnedZeldaInCutscene = false;
    } else if (inScriptedCutscene) {
        // Default behavior: don't intrude on story cutscenes with our own spawned Zelda unless the
        // user opts in via the "Show Zelda during cutscenes" toggle. This only ever affects our
        // own spawned actor -- cutscenes with their own story-placed HoZelda (e.g. the real
        // Ganondorf duel) are already handled above by the `storyHoZeldaActive` check.
        //
        // Unlike earlier, she is never deleted here: the `HoZeldaSetMatrix` hook instead hides her
        // in place (moved far underground and shrunk to an imperceptible size) for as long as
        // `s_hideSpawnedZeldaInCutscene` stays true, so she's back exactly where she belongs the
        // instant the cutscene ends with no recreation needed. If she doesn't already exist by the
        // time a cutscene starts, she simply stays unspawned for its duration (see below) rather
        // than being created mid-cutscene, which isn't safe (see `spawn_zelda_on_horse()`'s
        // callers' comments).
        //
        // A few specific cutscenes (`is_always_hidden_cutscene()`) are staged around Epona not
        // visibly carrying a second rider at all, so this mod's Zelda stays hidden through those
        // regardless of the toggle -- she's un-hidden again the instant the cutscene ends, same
        // as any other cutscene, since this whole branch only runs while `inScriptedCutscene`.
        s_hideSpawnedZeldaInCutscene = !show_zelda_in_cutscenes() || is_always_hidden_cutscene();
    } else if (horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda) {
        // Keep Zelda riding along on Epona at all times during ordinary gameplay, even after Link
        // dismounts: if nobody (neither the story nor this mod) currently has her attached to the
        // horse, spawn her. `daHoZelda_c::execute()` attaches itself to the current horse every
        // tick (`horse->setZeldaActor(this)`), and `daHoZelda_c::setMatrix()`/`setRideOffset()`/
        // `setAnm()` already support her sitting alone at the front of the saddle whenever Link
        // isn't riding (the `mIsSingleRide` case), so once created it takes care of the rest on its
        // own: reins, dual-ride animation blending, and the solo idle animation.
        s_hideSpawnedZeldaInCutscene = false;
        spawn_zelda_on_horse(horse);
    } else {
        s_hideSpawnedZeldaInCutscene = false;
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

    log_cutscene_diagnostics_if_changed(horse, is_title_screen(), inScriptedCutscene,
                                         storyHoZeldaActive);

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
