#include <cctype>
#include <cstdlib>
#include <cstring>

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

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

static ConfigVarHandle g_cvarShowInCutscenes = 0;

static bool show_zelda_in_cutscenes() {
    bool value = false;
    if (g_cvarShowInCutscenes == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarShowInCutscenes, &value) != MOD_OK) {
        return false;
    }
    return value;
}

static ConfigVarHandle g_cvarAutoTargetEnemies = 0;

static bool auto_target_enemies_enabled() {
    bool value = false;
    if (g_cvarAutoTargetEnemies == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarAutoTargetEnemies, &value) != MOD_OK) {
        return false;
    }
    return value;
}

static constexpr bool kDiagnosticLoggingEnabled = false;

static const char* kHoZeldaStageName = "HoZelda";

static ActorId s_spawnedZeldaId = 0;
static bool s_hasSpawnedZelda = false;

// TEMPORARY diagnostics for the still-unidentified Light Arrow rearing trigger; see usage near
// the setSingleAnime/arrowShooting hooks below. Remove once the trigger is confirmed and handled.
static int s_arrowShotDiagFramesLeft = 0;
static constexpr int kArrowShotDiagFrames = 180;  // ~3 seconds at 60fps

static s32 s_lastKnownZeldaRoomNo = -1;
static bool s_zeldaLostInSameRoom = false;

// Whether this mod is currently allowed to create/modify anything attached to Epona.
// If the mod is enabled while Epona is already loaded (e.g. toggled on mid-session via
// the mods menu), she wasn't just (re)created through her normal lifecycle, and
// spawning our HoZelda actor on her immediately can crash the game. So instead, the
// first time mod_update() runs we check whether Epona is already loaded; if she is, we
// withhold all changes to her until she next unloads and reloads via a real scene
// transition (the normal, known-safe path this mod already relies on elsewhere).
static bool s_safeToModifyEpona = false;
static bool s_observedInitialHorseState = false;

static fopAc_ac_c* find_spawned_zelda() {
    if (!s_hasSpawnedZelda) {
        return nullptr;
    }

    if (fpcM_IsCreating((fpc_ProcID)s_spawnedZeldaId)) {
        return nullptr;
    }

    fopAc_ac_c* actor = nullptr;
    fopAcM_SearchByID((fpc_ProcID)s_spawnedZeldaId, &actor);
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_HOZELDA_e) {
        s_hasSpawnedZelda = false;
        return nullptr;
    }
    return actor;
}

static bool is_spawned_zelda(const fopAc_ac_c* actor) {
    return s_hasSpawnedZelda && actor != nullptr &&
           (ActorId)fopAcM_GetID(actor) == s_spawnedZeldaId;
}

static bool is_spawned_zelda_attached(daHorse_c* horse) {
    fopAc_ac_c* attached = horse->getZeldaActor();
    if (!s_hasSpawnedZelda || attached == nullptr) {
        return false;
    }
    fopAc_ac_c* live = nullptr;
    fopAcM_SearchByID((fpc_ProcID)s_spawnedZeldaId, &live);
    return live != nullptr && live == attached;
}

// Whether the spawned Zelda actor is currently solo-riding the horse, checked via her own live
// actor (found by ID, exactly like find_spawned_zelda()) rather than horse->getZeldaActor().
//
// on_horse_call_substance_pre() below intentionally nulls out horse->m_zeldaActorKeep every tick
// (as part of the grass-whistle fix) and nothing ever restores it afterwards -- vanilla
// daHorse_c::execute() only re-attaches her once, at the very start of its own next tick. Any
// check reached after callHorseSubstance() runs within the same execute() call -- which includes
// every later stage of execute() (procWait()/checkTgHitTurn()/procTurnInit()/setNeckAnime(), all
// reached further down in the same function body), plus our own execute() post-hook -- was
// therefore reading horse->getZeldaActor() == nullptr, making is_spawned_zelda_attached(horse)
// (and anything gated on it, like epona_combat_reactions_suppressed()) silently and permanently
// read false during exactly the combat window this mod needs to detect. This is the actual root
// cause behind every "Epona still rears" report this session: none of the suppression hooks ever
// activated, because the gate itself never read true in the first place.
static bool is_spawned_zelda_riding_solo() {
    fopAc_ac_c* actor = find_spawned_zelda();
    return actor != nullptr && static_cast<daHoZelda_c*>(actor)->checkSingleRide();
}

static bool s_hideSpawnedZeldaInCutscene = false;

static bool is_title_screen() {
    scene_class* playScene = fopScnM_SearchByID(dStage_roomControl_c::getProcID());
    return playScene != nullptr && fpcM_GetName(playScene) == fpcNm_OPENING_SCENE_e;
}

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
        "demo02",
        "Demo36_01",
        "Demo36_02",
        "demo90",
    };
    const char* eventName = dComIfGp_getEventManager().getRunEventName();
    for (const char* name : kAlwaysHiddenDemoNames) {
        if (names_equal_case_insensitive(eventName, name)) {
            return true;
        }
    }
    return false;
}

struct CutsceneDiagnosticsSnapshot {
    bool horseDemoMode = false;
    bool titleScreen = false;
    bool storyHoZeldaActive = false;
    bool inScriptedCutscene = false;
    bool alwaysHiddenCutscene = false;
    bool showInCutscenesToggle = false;
    bool hideSpawnedZelda = false;
    bool hasSpawnedZelda = false;
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
               hideSpawnedZelda == other.hideSpawnedZelda &&
               hasSpawnedZelda == other.hasSpawnedZelda && demoStaffId == other.demoStaffId &&
               roomNo == other.roomNo && layerNo == other.layerNo &&
               std::strcmp(eventName, other.eventName) == 0 &&
               std::strcmp(demoArcName, other.demoArcName) == 0 &&
               std::strcmp(cutName, other.cutName) == 0;
    }
    bool operator!=(const CutsceneDiagnosticsSnapshot& other) const { return !(*this == other); }
};

static void log_cutscene_diagnostics_if_changed(daHorse_c* horse, bool titleScreen,
                                                 bool inScriptedCutscene,
                                                 bool storyHoZeldaActive) {
    if (!kDiagnosticLoggingEnabled) {
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
    snapshot.hasSpawnedZelda = s_hasSpawnedZelda;
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
        "alwaysHiddenCutscene={} showInCutscenesToggle={} hideSpawnedZelda={} "
        "hasSpawnedZelda={}",
        snapshot.eventName, snapshot.demoArcName, snapshot.cutName, snapshot.demoStaffId,
        snapshot.roomNo, snapshot.layerNo, snapshot.horseDemoMode, snapshot.titleScreen,
        snapshot.storyHoZeldaActive, snapshot.inScriptedCutscene, snapshot.alwaysHiddenCutscene,
        snapshot.showInCutscenesToggle, snapshot.hideSpawnedZelda, snapshot.hasSpawnedZelda);
}

static void* judge_other_hozelda(fopAc_ac_c* i_actor, void*) {
    if (fopAcM_GetName(i_actor) != fpcNm_HOZELDA_e || is_spawned_zelda(i_actor)) {
        return NULL;
    }
    return i_actor;
}

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

DEFINE_HOOK(static_cast<s32 (*)(fopAc_ac_c*)>(&fopAcM_delete), ActorDelete);

static HookAction on_actor_delete_pre(ModContext*, void* args, void*, void*) {
    if (!kDiagnosticLoggingEnabled) {
        return HOOK_CONTINUE;
    }

    fopAc_ac_c* actor = mods::arg<fopAc_ac_c*>(args, 0);
    if (s_hasSpawnedZelda && actor != nullptr &&
        (ActorId)fopAcM_GetID(actor) == s_spawnedZeldaId) {
        daHorse_c* horse = dComIfGp_getHorseActor();
        mods::log::warn(
            "spawned HoZelda actor id {} is being deleted via fopAcM_delete; event='{}' "
            "horseDemoMode={} room={} layer={}",
            (int)s_spawnedZeldaId, dComIfGp_getEventManager().getRunEventName(),
            horse != nullptr && horse->checkHorseDemoMode(), dComIfGp_roomControl_getStayNo(),
            dComIfG_play_c::getLayerNo(0));
    }
    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daHorse_c::callHorseSubstance, HorseCallSubstance);

static HookAction on_horse_call_substance_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    if (is_spawned_zelda_attached(horse)) {
        horse->setZeldaActor(nullptr);
    }
    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daHoZelda_c::setMatrix, HoZeldaSetMatrix);

static const f32 kHiddenScale = 0.0001f;

static void hide_zelda_visually(daHoZelda_c* zelda) {
    mDoMtx_stack_c::transS(zelda->current.pos);
    mDoMtx_stack_c::ZXYrotM(zelda->shape_angle.x, zelda->shape_angle.y, zelda->shape_angle.z);
    mDoMtx_stack_c::scaleM(cXyz(kHiddenScale, kHiddenScale, kHiddenScale));
    zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
}

static void on_hozelda_set_matrix_post(ModContext*, void* args, void*, void*) {
    static f32 s_turnStandBlend = 0.0f;

    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!s_hasSpawnedZelda || (ActorId)fopAcM_GetID(zelda) != s_spawnedZeldaId) {
        s_turnStandBlend = 0.0f;
        return;
    }

    daPy_py_c* link = daPy_getLinkPlayerActorClass();
    if (link != nullptr) {
        link->offHorseZelda();
    }

    if (s_hideSpawnedZeldaInCutscene) {
        if (zelda->model != nullptr) {
            hide_zelda_visually(zelda);
        }
        s_turnStandBlend = 0.0f;
        return;
    }

    bool dualRide = daPy_getLinkPlayerActorClass()->checkHorseRide();

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || zelda->model == nullptr) {
        s_turnStandBlend = 0.0f;
        return;
    }

    if (dualRide) {
        s_turnStandBlend = 0.0f;
    } else {
        cLib_chaseF(&s_turnStandBlend, horse->checkTurnStand() ? 1.0f : 0.0f, 1.0f / 8.0f);
    }

    bool inCutscene = horse->checkHorseDemoMode();

    if (!inCutscene && (dualRide || s_turnStandBlend <= 0.0f)) {
        return;
    }

    static const Vec kZeldaFrontHorseRidePos = {-75.893997f, 57.61f, 4.079f};

    static const Vec kLinkHorseRidePos = {-68.208984f, 41.609924f, 0.883789f};

    static const Vec kZeldaDualRidePos = {-5.894f, 52.61f, 4.079f};

    Vec localPos = dualRide ? kZeldaDualRidePos : kZeldaFrontHorseRidePos;
    if (!dualRide && s_turnStandBlend > 0.0f) {
        localPos.x += (kLinkHorseRidePos.x - kZeldaFrontHorseRidePos.x) * s_turnStandBlend;
        localPos.y += (kLinkHorseRidePos.y - kZeldaFrontHorseRidePos.y) * s_turnStandBlend;
        localPos.z += (kLinkHorseRidePos.z - kZeldaFrontHorseRidePos.z) * s_turnStandBlend;

        mDoMtx_multVec(horse->getRootMtx(), &localPos, &zelda->current.pos);
    }

    zelda->shape_angle = horse->shape_angle;
    zelda->current.angle.y = zelda->shape_angle.y;

    if (inCutscene) {
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

DEFINE_HOOK(&daHoZelda_c::setBowModel, HoZeldaSetBowModel);

static void hide_bow_visually(daHoZelda_c* zelda) {
    mDoMtx_stack_c::transS(zelda->current.pos);
    mDoMtx_stack_c::ZXYrotM(zelda->shape_angle.x, zelda->shape_angle.y, zelda->shape_angle.z);
    mDoMtx_stack_c::scaleM(cXyz(kHiddenScale, kHiddenScale, kHiddenScale));
    zelda->mpBowModel->setBaseTRMtx(mDoMtx_stack_c::get());
    // setBowModel() already called calc() with the original (visible) base matrix before this
    // post-hook runs, baking it into the model's joint matrices. Overriding the base matrix
    // alone has no visible effect until calc() re-runs with the shrunk matrix.
    zelda->mpBowModel->calc();
}

static void on_hozelda_set_bow_model_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda) || zelda->mpBowModel == nullptr) {
        return;
    }

    // When Zelda isn't hidden for a cutscene (that already shrinks her bow along with
    // the rest of her body), shrink just the bow whenever the "Zelda active in combat"
    // toggle is off, so it doesn't visibly float around on an otherwise unarmed Zelda.
    if (!s_hideSpawnedZeldaInCutscene && !auto_target_enemies_enabled()) {
        hide_bow_visually(zelda);
    }
}

static const f32 kAutoTargetRange = 4000.0f;

struct HoZeldaTargetSearch {
    const fopAc_ac_c* self;
    f32 maxDistSq;
    s16 maxAngle;
    fopAc_ac_c* best;
    f32 bestDistSq;
};

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

static void* judge_nearest_enemy(fopAc_ac_c* i_actor, void* i_data) {
    HoZeldaTargetSearch* search = static_cast<HoZeldaTargetSearch*>(i_data);

    if (i_actor == search->self || fopAcM_GetGroup(i_actor) != fopAc_ENEMY_e ||
        fopAcM_GetName(i_actor) == fpcNm_E_WB_e || fopAcM_GetName(i_actor) == fpcNm_E_DB_e ||
        fopAcM_GetName(i_actor) == fpcNm_E_YD_e || fopAcM_GetName(i_actor) == fpcNm_E_HB_e ||
        fopAcM_GetName(i_actor) == fpcNm_E_YH_e || fopAcM_GetName(i_actor) == fpcNm_E_GB_e ||
        i_actor->health <= 0 || is_dormant_enemy(i_actor))
    {
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

DEFINE_HOOK(&daHoZelda_c::execute, HoZeldaExecute);

static HookAction on_hozelda_execute_pre(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda)) {
        return HOOK_CONTINUE;
    }

    // TEMPORARY: tick down the Light-Arrow-flight diagnostic window armed in
    // on_arrow_shooting_post. Remove together with the rest of the epona-diag code.
    if (s_arrowShotDiagFramesLeft > 0) {
        s_arrowShotDiagFramesLeft--;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    bool inCutscene = horse != nullptr && horse->checkHorseDemoMode() && !is_title_screen();

    if (auto_target_enemies_enabled() && !inCutscene) {
        zelda->mGndAcKeep.setData(find_nearest_enemy(zelda));
    } else {
        zelda->mGndAcKeep.clearData();
    }
    return HOOK_CONTINUE;
}

// Forces the reins back to their default, saddle-anchored position, even though
// daHorse_c::setReinPosNormal() normally refuses to do anything while a single-riding Zelda is
// still attached to the horse (deferring to her hand-following position by design). Briefly
// detaching her around the call forces it through with no lasting effect, since execute()
// unconditionally reattaches her at the very start of next tick regardless (the same trick used
// for the grass-whistle and cutscene-hiding fixes elsewhere in this file).
static void force_reins_to_normal_pos(daHoZelda_c* zelda, daHorse_c* horse) {
    if (horse->getZeldaActor() == static_cast<fopAc_ac_c*>(zelda)) {
        horse->setZeldaActor(nullptr);
    }
    horse->setReinPosNormal();
}

static void on_hozelda_execute_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda) || !s_hideSpawnedZeldaInCutscene) {
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr) {
        return;
    }

    force_reins_to_normal_pos(zelda, horse);
}

// Riding solo, daHorse_c::execute() has Zelda's own daHoZelda_c::execute() call
// horse->setReinPosHand() every tick, unconditionally re-anchoring the reins to her hand joint
// regardless of what she's doing. setReinPosHandSubstance() is a rope-physics simulation (gravity
// + inertia, not an instant snap): letting it run even once with a "hand" target and then
// immediately overriding with setReinPosNormal() right after (as an execute() post-hook once did)
// still leaves its spring/velocity state tainted by that one hand-aimed step every single frame,
// which visibly kept the reins jittering during the whole bow sequence instead of settling into
// the same still default position they'd have with nobody riding. Replacing the hand-tracking
// call outright for the duration of the bow sequence, rather than patching up its result
// afterwards, means only the normal/default physics step ever runs during that time.
DEFINE_HOOK(&daHorse_c::setReinPosHandSubstance, HorseSetReinPosHandSubstance);

static HookAction on_horse_set_rein_pos_hand_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    daHoZelda_c* zelda = horse->getZeldaActor();
    if (zelda == nullptr || !is_spawned_zelda(zelda) || zelda->mBowMode == 0) {
        return HOOK_CONTINUE;
    }

    force_reins_to_normal_pos(zelda, horse);
    return HOOK_SKIP_ORIGINAL;
}

static constexpr u8 kArrowWaitFrames = 5;

static constexpr f32 kDrawAnimSpeedMultiplier = 2.0f;

// While Zelda is actively fighting from horseback (auto-target enabled), Epona reacting to the
// fight at all -- rearing at a nearby enemy, or flinching from Zelda's own Light Arrows flying
// close to her -- is counterproductive, so all of it is suppressed outright. Outside of that
// feature, Epona keeps her normal reactions (e.g. a player who swings a sword at her by accident
// should still see her react), since only Zelda's own combat is what's being worked around here.
static bool epona_combat_reactions_suppressed(daHorse_c* horse) {
    // horse is kept for call-site symmetry with the hooks that gate on it, but the live
    // ride-state check below (unlike the old horse->getZeldaActor()-based one) doesn't need it.
    (void)horse;
    return auto_target_enemies_enabled() && is_spawned_zelda_riding_solo();
}

// An exception to the suppression above: if the *player* (not Zelda's own combat) is the one
// physically hitting Epona while dismounted, she should still rear up and interrupt whatever
// Zelda is doing, exactly like she would without this mod -- only Zelda's own Light-Arrow/enemy
// combat is meant to be worked around here. GetTgHitAc() reports whichever actor's own attack
// collider actually caused a given target cylinder/sphere's hit, independent of ChkTgHit()'s
// generic "something touched me" bit, so it can distinguish the player's sword from an arrow or
// a nearby enemy. Checked in on_horse_execute_pre (before anything clears the hit flags) and
// latched into s_allowRearForDismountedPlayerHit for the duration of the resulting reaction.
static bool is_dismounted_player_tg_hit(daHorse_c* horse) {
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    bool dismounted = player != nullptr && !player->checkHorseRide();
    fopAc_ac_c* playerActor = dismounted ? static_cast<fopAc_ac_c*>(player) : nullptr;

    bool anyTgHit = horse->m_tgco_cyl[0].ChkTgHit() || horse->m_tgco_cyl[1].ChkTgHit() ||
                    horse->m_tgco_cyl[2].ChkTgHit() || horse->m_head_sph.ChkTgHit();
    bool result = dismounted &&
                  ((horse->m_tgco_cyl[0].ChkTgHit() && horse->m_tgco_cyl[0].GetTgHitAc() == playerActor) ||
                      (horse->m_tgco_cyl[1].ChkTgHit() && horse->m_tgco_cyl[1].GetTgHitAc() == playerActor) ||
                      (horse->m_tgco_cyl[2].ChkTgHit() && horse->m_tgco_cyl[2].GetTgHitAc() == playerActor) ||
                      (horse->m_head_sph.ChkTgHit() && horse->m_head_sph.GetTgHitAc() == playerActor));

    // TEMPORARY diagnostic: only logs on a real hit tick (cheap, rare), to pin down why a
    // genuine dismounted player hit might not be recognized as such.
    if (anyTgHit) {
        mods::log::warn(
            "[epona-diag] is_dismounted_player_tg_hit: player={:p} dismounted={} playerActor={:p} "
            "result={} | cyl0(hit={},ac={:p}) cyl1(hit={},ac={:p}) cyl2(hit={},ac={:p}) "
            "head(hit={},ac={:p})",
            (void*)player, dismounted, (void*)playerActor, result,
            (bool)horse->m_tgco_cyl[0].ChkTgHit(), (void*)horse->m_tgco_cyl[0].GetTgHitAc(),
            (bool)horse->m_tgco_cyl[1].ChkTgHit(), (void*)horse->m_tgco_cyl[1].GetTgHitAc(),
            (bool)horse->m_tgco_cyl[2].ChkTgHit(), (void*)horse->m_tgco_cyl[2].GetTgHitAc(),
            (bool)horse->m_head_sph.ChkTgHit(), (void*)horse->m_head_sph.GetTgHitAc());
    }

    return result;
}

// Latched true by on_horse_execute_pre the moment a genuine dismounted-player hit is seen (via
// is_dismounted_player_tg_hit()), and consulted by every other suppression point below for as
// long as it stays set. ChkTgHit() is only a one-tick pulse, but the resulting reaction
// (rearing/turning) plays out over many subsequent frames, so this can't simply be recomputed
// fresh each tick -- it has to persist across the whole reaction. Cleared again by
// on_horse_execute_post once she's confirmed back to a calm, non-reacting state.
static bool s_allowRearForDismountedPlayerHit = false;

// daHorse_c's ANM_HS_* animation indices aren't exposed in its header (they're #defines local to
// d_a_horse.cpp), so the handful needed here are duplicated by value. Confirmed against
// d_a_hozelda.cpp's own raw-literal horse_anm_idx checks (e.g. its "0x14" is this ANM_HS_STAND).
static constexpr u16 kAnmHorseStand = 20;       // ANM_HS_STAND: rearing "stand" pose
static constexpr u16 kAnmHorseExcitement = 8;   // ANM_HS_EXCITEMENT: spooked-by-enemy reaction
static constexpr u16 kAnmHorseExcitement2 = 9;  // ANM_HS_EXCITEMENT_2: same, alternate clip
static constexpr u16 kAnmHorseWait01 = 27;      // ANM_HS_WAIT_01: normal idle pose
static constexpr u16 kAnmHorseLash = 14;        // ANM_HS_LASH: neck-toss "startled" reaction

static void set_anm_auto_target(daHoZelda_c* zelda) {
    u16 anm_idx[3];
    daHorse_c* horse = (daHorse_c*)dComIfGp_getHorseActor();
    daPy_py_c* player = daPy_getLinkPlayerActorClass();

    if (horse == NULL) {
        return;
    }

    // Even with every known trigger above suppressed, Epona's own m_anmIdx[0] can still read back
    // ANM_HS_STAND (0x14) for a stray tick: the hit that provokes it is cleared and her engine
    // state is corrected the same tick (see HorseExecute's pre/post hooks below), but this
    // function runs from her rider's own execute(), which can observe that still-dirty value in
    // the narrow window before her own correction visibly takes effect. Vanilla's own mapping
    // below treats 0x14 as a meaningful "look alarmed" cue -- fine normally, but while Zelda rides
    // solo it collapses her into the plain "not aiming" branch further down, which resets her
    // upper-body anim and deletes her in-flight arrow, restarting her whole draw/fire cycle from
    // scratch and making her seem to fire much faster than intended. Treat it as a no-op idle tick
    // instead, same as vanilla already does for ANM_HS_EXCITEMENT/EXCITEMENT_2 (8/9, which fall
    // through to the same default case below and were never a problem).
    //
    // That said, when a dismounted player's hit is genuinely making Epona rear
    // (s_allowRearForDismountedPlayerHit), ANM_HS_STAND is a real, full reaction -- not a stray
    // glitch tick -- so Zelda should play her own normal "look alarmed" reaction (0x16) to it
    // too, exactly like she would without this mod, rather than being held in the no-op idle pose
    // and clipping through Epona as she rears.
    bool reactionsSuppressed =
        epona_combat_reactions_suppressed(horse) && !s_allowRearForDismountedPlayerHit;

    u16* anm_p = anm_idx;
    for (int i = 0; i < 3; i++, anm_p++) {
        u16 horse_anm_idx = horse->getAnmIdx(i);
        if (horse_anm_idx == 0x14 && reactionsSuppressed) {
            *anm_p = 0x1C;
        } else if (horse_anm_idx == 0x14) {
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
            *anm_p = 0xE;
        } else if (horse_anm_idx == 0xFFFF) {
            *anm_p = 0xFFFF;
        } else {
            *anm_p = 0x1C;
        }
    }

    fopAc_ac_c* target_actor = zelda->mGndAcKeep.getActor();

    b_gnd_class* ganondorf = (target_actor != NULL && fopAcM_GetName(target_actor) == fpcNm_B_GND_e)
                                 ? static_cast<b_gnd_class*>(target_actor)
                                 : NULL;

    int target_seen_angleY = 0;
    if (target_actor != NULL) {
        target_seen_angleY = fopAcM_seenActorAngleY(zelda, target_actor);
    }

    bool target_vulnerable = false;
    if (ganondorf != NULL) {
        target_vulnerable = ganondorf->checkPiyo() != 1;
    } else if (target_actor != NULL) {
        target_vulnerable = target_actor->health > 0;
    }

    if ((anm_idx[0] == 0xE || anm_idx[0] == 0x1C) && zelda->field_0x6da == 0 && !zelda->mDamageInit &&
        zelda->field_0x6dd == 0 && target_actor != NULL && target_vulnerable &&
        (target_seen_angleY < zelda->mpHIO->m.bow_start_angle ||
            (zelda->mBowMode != 0 && target_seen_angleY < zelda->mpHIO->m.bow_end_angle)))
    {
        zelda->mBowMode = 1;
    } else {
        zelda->mBowMode = 0;
    }

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

    // Normally only the two shared "riding together" idle anims (0xE/0x1C) are allowed into the
    // bow-aiming branch below; solo riding always overrides anm_idx[0] to one of its own idle
    // poses (0x17/0x11/0x1B) above, which would otherwise fall through to the plain "not aiming"
    // branch here and silently disable auto-target while Zelda is alone on Epona. Let a solo ride
    // with an active bow target through as well, so she can still draw and fire on her own.
    bool soloBowMode = zelda->mIsSingleRide && zelda->mBowMode != 0;

    if (((anm_idx[0] != 0x1C && anm_idx[0] != 0xE) && !soloBowMode) || player->checkHorseElecDamage()) {
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
        // Riding double, the bow sequence has its own dedicated sit-and-aim base pose (0x1A).
        // Riding solo there's no such pose, so keep her already-computed solo idle anim as the
        // base instead; the bow draw/fire motion below is layered on top as an upper-body-only
        // overlay anim (see setUpperAnime()'s joint-range blend), so her seated position and legs
        // still match normal solo riding while just her arms/torso play the aiming animation.
        if (!zelda->mIsSingleRide) {
            anm_idx[0] = 0x1A;
        }
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

// Vanilla setNeckAngle() aims Zelda's neck/eyes at Link whenever he isn't riding Epona --
// normally only true for a few story moments where he's approaching her on foot (e.g. right
// before mounting up). Under this mod, "not riding" also describes virtually all of ordinary
// solo-riding combat, since Zelda stays mounted alone while Link fights on foot nearby, so her
// head and eyes kept locking onto Link's position the whole time instead of whatever enemy she
// was actually drawing and firing her bow at. Reuse the same target her aiming/body-angle logic
// already tracks (mGndAcKeep, populated every tick by on_hozelda_execute_pre's auto-target
// search) whenever she's riding solo and one is in range, falling back to vanilla's own
// player/Midna-attention-pos behavior otherwise -- e.g. with no target in range, auto-targeting
// disabled, or actually riding double with Link.
static void set_neck_angle_auto_target(daHoZelda_c* zelda) {
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    fopAc_ac_c* target = zelda->mGndAcKeep.getActor();
    cXyz* var_r28 = NULL;
    s16 angle_x_target = 0;
    s16 angle_y_target = 0;
    s16 var_r27 = 0;
    s16 var_r26 = 0;

    if (target != NULL && zelda->checkSingleRide()) {
        var_r28 = &target->eyePos;
    } else if (!player->checkHorseRide()) {
        cXyz sp34 = player->current.pos - zelda->current.pos;
        int sp18 = cLib_distanceAngleS(sp34.atan2sX_Z(), zelda->shape_angle.y);

        f32 var_f31;
        int sp14;
        if (zelda->mNeckAngle.y != 0) {
            sp14 = 0x5800;
            var_f31 = 250000.0f;
        } else {
            sp14 = 0x5000;
            var_f31 = 90000.0f;
        }

        if (sp18 <= 0x4000 || (sp18 <= sp14 && sp34.abs2XZ(player->current.pos) < var_f31)) {
            var_r28 = &player->eyePos;
        }
    } else if (zelda->mBowMode == 0 && zelda->field_0x6da == 0 && player->checkMidnaAtnPos()) {
        var_r28 = player->getMidnaAtnPos();
    }

    if (var_r28 != NULL) {
        cXyz sp1C = *var_r28 - zelda->field_0x6f8;
        s16 spE = sp1C.atan2sY_XZ();
        s16 spC = sp1C.atan2sX_Z() - zelda->shape_angle.y;
        s16 spA = spE;
        s16 sp8 = spC;

        angle_x_target = cLib_minMaxLimit<s16>((s16)spE, -10000, 8000);
        angle_y_target = cLib_minMaxLimit<s16>((s16)spC, -20000, 20000);

        var_r27 = angle_x_target >> 1;
        var_r26 = angle_y_target >> 1;

        angle_x_target = var_r27;
        angle_y_target = var_r26;

        var_r27 = (s16)(var_r27 + (spA - spE));
        var_r26 = (s16)(var_r26 + (sp8 - spC));
    }

    daPy_addCalcShort(&zelda->mNeckAngle.x, angle_x_target, 3, 0x1000, 0x100);
    daPy_addCalcShort(&zelda->mNeckAngle.y, angle_y_target, 3, 0x1000, 0x100);

    if (zelda->field_0x6de != 0) {
        zelda->clearEyeMove();
    } else {
        zelda->setEyeMove(var_r28, var_r27, var_r26);
    }
}

DEFINE_HOOK(&daHoZelda_c::setNeckAngle, HoZeldaSetNeckAngle);

static void on_hozelda_set_neck_angle_replace(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda)) {
        HoZeldaSetNeckAngle::g_orig(zelda);
        return;
    }

    set_neck_angle_auto_target(zelda);
}

// Zelda already knows Link is the hero in wolf form; she shouldn't stop him from
// transforming back and forth just because she happens to be looking at him.
DEFINE_HOOK_SYMBOL("daMidna_searchNpc", void*(fopAc_ac_c*, void*), MidnaSearchNpc);

static HookAction on_midna_search_npc_pre(ModContext*, void* args, void*, void*) {
    fopAc_ac_c* actor = mods::arg<fopAc_ac_c*>(args, 0);
    if (is_spawned_zelda(actor)) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

// Epona's idle-state AI (daHorse_c::procWait) periodically scans for nearby ENEMY-group actors
// via this judge function and, if one is found, spooks into a rearing "excitement" animation.
// It has no notion of Zelda riding solo and defending herself -- so whenever her auto-targeted
// Light Arrow combat brings an enemy within range, Epona rears up mid-fight as if under threat.
// Skip the search entirely (judge functions return NULL for "no match") while that reaction is
// suppressed.
DEFINE_HOOK_SYMBOL("daHorse_searchEnemy", void*(fopAc_ac_c*, void*), HorseSearchEnemy);

static HookAction on_horse_search_enemy_pre(ModContext*, void*, void*, void*) {
    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse != nullptr && epona_combat_reactions_suppressed(horse)) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

// Separately, daHorse_c::checkTgHitTurn() (called every idle tick while unridden, the same
// state Zelda's solo riding puts Epona in) spins her around into a rearing stand-up reaction
// whenever her own body collision cylinders (m_tgco_cyl/m_head_sph) register a hit -- which
// happens whenever ANY attack collider physically touches her, including a Light Arrow's own
// attack cylinder simply flying close enough to graze her hitbox on its way to its target, with
// no regard for who's firing it or why.
//
// checkTgHitTurn() itself has only two, tiny, same-file call sites, which the compiler may
// freely inline, making it an unreliable hook target in practice (its address may never actually
// be called at runtime). Both of its call sites -- and the equivalent one reached via
// daHorse_searchEnemy above -- funnel into daHorse_c::procTurnInit(1) to actually play the
// rearing ANM_HS_STAND animation, so that shared, many-call-site function (unlikely to be
// inlined away) is hooked directly instead, gated on its "was this a startled reaction" param.
DEFINE_HOOK(&daHorse_c::procTurnInit, HorseProcTurnInit);

// TEMPORARY diagnostic helper: logs every "spooked" trigger this mod knows about, regardless of
// whether it was actually suppressed, so a report of Epona still rearing from a Light Arrow can
// be matched against what (if anything) fired here. Remove once the Light Arrow rearing trigger
// is confirmed and handled.
static void report_diag(const char* source, bool suppressed, bool autoTarget, bool zeldaAttached,
    bool singleRide) {
    mods::log::warn("[epona-diag] {} fired (suppressed={}, autoTarget={}, zeldaAttached={}, "
                     "singleRide={})",
        source, suppressed, autoTarget, zeldaAttached, singleRide);
}

static HookAction on_horse_proc_turn_init_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    int isStartledTurn = mods::arg<int>(args, 1);
    bool suppressed = epona_combat_reactions_suppressed(horse);

    // TEMPORARY, broader testing step: the narrower "only skip when isStartledTurn != 0" gating
    // below didn't stop a full, multi-frame rearing animation from playing out in-game, so as a
    // diagnostic experiment every procTurnInit() call (regardless of its param) is now logged and
    // skipped while suppressed, to directly confirm whether this function is really the one
    // responsible for what's being observed. This is intentionally broader than production-safe:
    // it will also block Epona's normal directional turning while solo-ridden in combat mode.
    // Narrow this back down (or replace it) once confirmed.
    //
    // Exception: if a dismounted player is the one physically hitting her this tick (see
    // is_dismounted_player_tg_hit()), let the real rearing reaction through instead of blocking
    // it, since that's meant to still interrupt Zelda regardless of her own combat.
    if (suppressed && !s_allowRearForDismountedPlayerHit) {
        mods::log::warn(
            "[epona-diag] procTurnInit(isStartledTurn={}) fired while suppressed -- skipping "
            "unconditionally for testing (current anmIdx={})",
            isStartledTurn, horse->getAnmIdx(0));
        return HOOK_SKIP_ORIGINAL;
    }

    if (isStartledTurn != 0) {
        report_diag("procTurnInit(1)", suppressed, auto_target_enemies_enabled(),
            find_spawned_zelda() != nullptr, is_spawned_zelda_riding_solo());
    }

    return HOOK_CONTINUE;
}

// Rather than keep chasing individual internal trigger paths one at a time, hook the one place
// all of them must funnel through to become visible: daHorse_c::setSingleAnime(), and refuse to
// switch her into any of the three "spooked" animations above while her combat reactions are
// suppressed.
DEFINE_HOOK(&daHorse_c::setSingleAnime, HorseSetSingleAnime);

// The window itself (s_arrowShotDiagFramesLeft/kArrowShotDiagFrames) is declared near the top of
// this file alongside the other spawn-tracking statics, armed in on_arrow_shooting_post below and
// ticked down in on_hozelda_execute_pre. For a few seconds after Zelda's own Light Arrow is
// released (covering its full flight time), every daHorse_c::setSingleAnime() call is logged
// regardless of which animation it requests, in case some 4th, not-yet-identified animation index
// is ever involved outside the three handled above.
static HookAction on_horse_set_single_anime_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    u16 anmIdx = mods::arg<u16>(args, 1);
    bool isSpookedAnime = anmIdx == kAnmHorseStand || anmIdx == kAnmHorseExcitement ||
                           anmIdx == kAnmHorseExcitement2;
    if (isSpookedAnime) {
        bool suppressed = epona_combat_reactions_suppressed(horse);
        report_diag("setSingleAnime", suppressed, auto_target_enemies_enabled(),
            find_spawned_zelda() != nullptr, is_spawned_zelda_riding_solo());
        if (suppressed && !s_allowRearForDismountedPlayerHit) {
            return HOOK_SKIP_ORIGINAL;
        }
    } else if (s_arrowShotDiagFramesLeft > 0) {
        mods::log::warn("[epona-diag] setSingleAnime anmIdx={} (not one of the known spooked IDs, "
                         "frames left in arrow-shot window={})",
            anmIdx, s_arrowShotDiagFramesLeft);
    }

    return HOOK_CONTINUE;
}

// A fourth, separate reaction path lives directly in daHorse_c::execute() itself (not reached
// through procWait() at all): whenever she's actively being ridden (FLG0_UNK_1) and her neck-anim
// slot is free, she plays a neck-toss "startled" animation (ANM_HS_LASH) plus an alarmed voice
// clip the instant any of her body collision cylinders carry a "target hit" flag -- the exact
// same ChkTgHit() signal a nearby Light Arrow sets, gated by the exact same condition our
// ChkTgHit()-clearing pre-hook already targets. In principle that clearing should already prevent
// this from ever firing too, but it's a genuinely different call site (inside the large execute()
// function itself, not procWait()), where the compiler's inlining decisions could differ. As a
// direct, low-risk extra layer, block ANM_HS_LASH specifically (leaving her other legitimate idle
// neck-toss variants untouched) whenever her combat reactions are suppressed.
DEFINE_HOOK(&daHorse_c::setNeckAnime, HorseSetNeckAnime);

static HookAction on_horse_set_neck_anime_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    u16 anmIdx = mods::arg<u16>(args, 1);
    if (anmIdx != kAnmHorseLash) {
        return HOOK_CONTINUE;
    }

    bool suppressed = epona_combat_reactions_suppressed(horse);
    report_diag("setNeckAnime(ANM_HS_LASH)", suppressed, auto_target_enemies_enabled(),
        find_spawned_zelda() != nullptr, is_spawned_zelda_riding_solo());
    if (suppressed && !s_allowRearForDismountedPlayerHit) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);

// A diagnostic log confirmed none of the hooks above actually catch Light Arrow rearing (neither
// toast/log line ever fired), even though she really was found sitting in ANM_HS_STAND by the
// execute() safety net further down. The root cause: daHorse_c::checkTgHitTurn() (reached from
// procWait() while Zelda rides solo, since she's never registered as the vanilla "ridden" rider
// the way Link is) spins Epona into procTurnInit(1) -- and so does its setSingleAnime(ANM_HS_STAND)
// call -- whenever her own body collision cylinders (m_tgco_cyl/m_head_sph) carry a "target hit"
// flag, regardless of who or what set it. checkTgHitTurn() is a single-statement, same-file,
// two-call-site function tiny enough for the compiler to inline together with its tail call to
// procTurnInit() at those call sites specifically, silently bypassing hooks on both procTurnInit
// and setSingleAnime for exactly this trigger while still correctly intercepting other call sites
// (e.g. a player's sword swing, confirmed fixed by the procTurnInit hook above).
//
// Rather than keep hooking functions the compiler is free to inline away, intercept the actual
// stimulus instead: the "target hit" flag itself (ChkTgHit()) is a generic "something touched me"
// bit set by the collision engine whenever any attack collider overlaps hers, with no regard for
// who's attacking or why -- including a Light Arrow's own attack collider simply flying close
// enough to graze her hitbox. Clear it ourselves, every tick, before any of daHorse_c's own
// per-tick logic gets a chance to read it, pre-empting checkTgHitTurn() (and the equivalent check
// further down in execute() itself, used while actually ridden) from ever observing a hit at all.
DEFINE_HOOK(&daHorse_c::execute, HorseExecute);

static HookAction on_horse_execute_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    if (!epona_combat_reactions_suppressed(horse)) {
        return HOOK_CONTINUE;
    }

    // ChkTgHit() is only a one-tick pulse (set the instant a hit is detected, then cleared again),
    // but a genuine rearing reaction plays out over many subsequent frames. Latch the exemption
    // (instead of recomputing it fresh every tick) the moment a real hit is seen, and leave it set
    // for the rest of the reaction; on_horse_execute_post clears it again once she's confirmed
    // back to a calm, non-reacting state. Without this, every tick after the actual hit (once its
    // one-frame ChkTgHit() pulse has already expired) would see allow=false and the post-hook's
    // safety net would force her back to idle mid-animation.
    if (is_dismounted_player_tg_hit(horse)) {
        s_allowRearForDismountedPlayerHit = true;
    }
    if (s_allowRearForDismountedPlayerHit) {
        return HOOK_CONTINUE;
    }

    horse->m_tgco_cyl[0].ClrTgHit();
    horse->m_tgco_cyl[1].ClrTgHit();
    horse->m_tgco_cyl[2].ClrTgHit();
    horse->m_head_sph.ClrTgHit();

    return HOOK_CONTINUE;
}

// Belt-and-suspenders safety net, kept in case some other still-unidentified path sets her
// animation directly, bypassing every hook above: correct her back to a clean idle wait on any
// tick she's still found holding one of the three "spooked" anims while suppressed. Unlike a bare
// setSingleAnime() override, procWaitInit() also resets m_proc/m_procID back to procWait -- doing
// only the former while a startled turn's m_proc stayed on procTurn left the two fighting each
// other every tick (reported in-game as Epona rocking back and forth in place), since procTurn()
// kept expecting ANM_HS_STAND while this safety net forced her out of it right back out again.
//
// procWaitInit() alone still wasn't enough: it defers the actual anim correction to
// setMoveAnime(3.0f), which blends via a 3-frame morph (mDoExt_MtxCalcOldFrame::initOldFrameMorf)
// starting from whatever pose was already on screen -- i.e. the (partial) rearing/excitement pose
// from this very tick. That produced a few visibly-blended frames of Epona rearing before settling
// back to idle, exactly matching the reported "she rears up, then clips Zelda as she settles back
// down" regression. Forcing an explicit setSingleAnime() with i_morf=0.0f first snaps her straight
// to the idle pose with zero blend (confirmed: initOldFrameMorf() zeroes its morph rate entirely
// for any non-positive morf, instead of starting a new interpolation), so that by the time
// procWaitInit()'s own setMoveAnime(3.0f) runs afterwards, it blends from an already-idle pose
// (a harmless no-op blend) rather than from the spooked one.
static void on_horse_execute_post(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);

    u16 anmIdx = horse->getAnmIdx(0);
    bool isSpookedAnime = anmIdx == kAnmHorseStand || anmIdx == kAnmHorseExcitement ||
                          anmIdx == kAnmHorseExcitement2;

    // Beyond the anim index itself, also watch her top-level state machine directly:
    // procTurnInit() (the function our HorseProcTurnInit hook targets) is a small, single-purpose
    // function called from exactly two sites within the SAME translation unit as checkTgHitTurn()
    // (itself already confirmed, per the ChkTgHit()-clearing fix, to be inlined away at its own
    // call site inside procWait() -- see Cause 2). If procTurnInit() is similarly inlined at
    // either call site, our function-level hook on it can never fire for those calls, since they
    // never execute a real CALL instruction to its standalone address. setSingleAnime() is still a
    // real, large, widely-called function unlikely to be inlined, so its own hook (Cause 4) should
    // still block the visible anim switch to ANM_HS_STAND in that scenario -- but procTurnInit()
    // ALSO unconditionally sets m_proc/m_procID to procTurn/PROC_TURN_e as plain field writes
    // (not hookable function calls), so the state machine can silently flip into the turning state
    // even while the anim itself stays blocked. The following tick, procTurn() would then run
    // instead of procWait(), independently driving her body's turn-around regardless of anim
    // state. Catching m_procID == PROC_TURN_e directly (on top of the anim-index check) closes
    // this gap regardless of inlining.
    bool inTurnState = horse->m_procID == daHorse_c::PROC_TURN_e;

    if (!isSpookedAnime && !inTurnState) {
        // She's confirmed back to a calm, non-reacting state -- end the dismounted-player-hit
        // exemption window (see on_horse_execute_pre), so a later, unrelated Light Arrow/nearby
        // enemy trigger goes back to being suppressed as normal instead of staying permanently
        // exempt from a hit several seconds ago.
        s_allowRearForDismountedPlayerHit = false;
        return;
    }

    // Log unconditionally (even if epona_combat_reactions_suppressed() currently reads false)
    // whenever either condition is observed, so a report of the FULL rearing animation playing
    // out unimpeded (instead of a brief, corrected flash) can be checked against whether
    // suppression was actually active at that moment, or whether one of its three gating
    // conditions was itself false at the critical tick -- which would explain why none of the
    // entry-blocking hooks (searchEnemy/procTurnInit/setSingleAnime) or the ChkTgHit() clear ever
    // got a chance to engage in the first place.
    bool suppressed = epona_combat_reactions_suppressed(horse);
    bool autoTarget = auto_target_enemies_enabled();
    bool zeldaAttached = find_spawned_zelda() != nullptr;
    bool singleRide = is_spawned_zelda_riding_solo();
    mods::log::warn(
        "[epona-diag] execute() post-hook found anmIdx={} inTurnState={} (suppressed={}, "
        "autoTarget={}, zeldaAttached={}, singleRide={}, allowPlayerHit={})",
        anmIdx, inTurnState, suppressed, autoTarget, zeldaAttached, singleRide,
        s_allowRearForDismountedPlayerHit);

    if (suppressed && !s_allowRearForDismountedPlayerHit) {
        mods::log::warn(
            "[epona-diag] -- resetting back to her wait state. This means something bypasses the "
            "ChkTgHit()/setSingleAnime/procTurnInit suppression entirely (possibly via inlining); "
            "please report this log line if seen.");
        horse->setSingleAnime(kAnmHorseWait01, 1.0f, 0.0f, -1, 0.0f, 0);
        horse->procWaitInit();
    }
}

static void on_arrow_shooting_post(ModContext*, void* args, void*, void*) {
    daArrow_c* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow->mArrowType != daArrow_c::ARROW_TYPE_LIGHT || !auto_target_enemies_enabled()) {
        return;
    }

    daHoZelda_c* zelda = (daHoZelda_c*)arrow->field_0xa08.getActor();
    if (zelda == nullptr || fopAcM_GetName(zelda) != fpcNm_HOZELDA_e || !is_spawned_zelda(zelda)) {
        return;
    }

    // TEMPORARY diagnostic: mark the start of a real Light Arrow release from the mod's Zelda so
    // the setSingleAnime diagnostic above can watch for any horse animation change during its
    // flight. Remove together with that diagnostic once the rearing trigger is confirmed.
    s_arrowShotDiagFramesLeft = kArrowShotDiagFrames;
    mods::log::warn("[epona-diag] Light Arrow released, starting {}-frame watch window",
        kArrowShotDiagFrames);

    fopAc_ac_c* target = zelda->mGndAcKeep.getActor();
    if (target == nullptr) {
        return;
    }

    cXyz dir = target->eyePos - arrow->current.pos;
    if (dir.abs() < 1.0f) {
        return;
    }

    arrow->current.angle.x = -dir.atan2sY_XZ();
    arrow->current.angle.y = dir.atan2sX_Z();
    arrow->shape_angle.y = arrow->current.angle.y;

    dir.normalizeZP();
    arrow->speed = dir * arrow->field_0x99c;
}

DEFINE_HOOK(&daArrow_c::execute, ArrowExecute);

// daArrow_c::procWait() (run every tick while the arrow is still nocked, before release)
// unconditionally re-arms a short draw-suppression timer (field_0x942) whenever the player's
// camera has "attention status 2" set -- true first-person view, where the engine hides Link's
// own body model to avoid clipping against the near camera. That check never looks at which
// actor actually owns the arrow, so Zelda's own Light Arrow (o_zg_arow.bmd) goes invisible in
// first person too, for as long as the status stays set. It only happens to become visible
// again when the player also draws their own bow, since Link's bow-aim camera clears that same
// status as a side effect (to keep his own arms/bow visible while aiming) -- nothing to do with
// Zelda specifically. Force the timer back off for her own arrow every tick so it stays visible
// regardless of the player's own bow state.
static void on_arrow_execute_post(ModContext*, void* args, void*, void*) {
    daArrow_c* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow->mArrowType != daArrow_c::ARROW_TYPE_LIGHT) {
        return;
    }

    if (!is_spawned_zelda(arrow->field_0xa08.getActor())) {
        return;
    }

    arrow->field_0x942 = 0;
}

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

static const u8 kLightArrowResMgrId = 2;

static bool s_lightArrowJpcReady = false;
static JPAResourceManager* s_lightArrowResMgr = nullptr;

static const u32 kLightArrowHeapSize = 0x100000;

static void* s_lightArrowHeapBacking = nullptr;
static JKRExpHeap* s_lightArrowHeap = nullptr;

// Grows JPAEmitterManager::pResMgrAry/ridMax (hardcoded to 2 slots at engine boot) by one,
// so bank kLightArrowResMgrId can be registered. No-op if the array is already big enough.
static bool grow_particle_emitter_manager_for_light_arrow_bank(JPAEmitterManager* mgr) {
    if (mgr->ridMax > kLightArrowResMgrId) {
        return true;
    }

    u8 newRidMax = kLightArrowResMgrId + 1;
    JKRHeap* heap = g_dComIfG_gameInfo.play.getParticle()->getHeap();
    auto* newAry = JKR_NEW_ARRAY_ARGS(JPAResourceManager*, newRidMax, heap, 0);
    if (newAry == nullptr) {
        return false;
    }

    for (u8 i = 0; i < newRidMax; i++) {
        newAry[i] = (i < mgr->ridMax) ? mgr->pResMgrAry[i] : nullptr;
    }
    mgr->pResMgrAry = newAry;
    mgr->ridMax = newRidMax;
    return true;
}

// Loads Pscene181.jpc and registers it as resource bank kLightArrowResMgrId, growing the
// emitter manager's resource array first if needed. Idempotent: no-ops immediately once
// s_lightArrowJpcReady is true. Safe to call at any time after the mod is initialized --
// whether that's during the normal boot sequence (via the ParticleCreateCommon post-hook)
// or well after boot, if the mod is enabled mid-session once the particle system already
// exists (via mod_initialize()).
static void ensure_light_arrow_particles_loaded() {
    if (s_lightArrowJpcReady) {
        return;
    }

    dPa_control_c* control = g_dComIfG_gameInfo.play.getParticle();
    JPAEmitterManager* emitterMng = dPa_control_c::getEmitterManager();
    if (control == nullptr || control->mCommonResMng == nullptr || emitterMng == nullptr) {
        // The particle system hasn't finished dPa_control_c::createCommon() yet -- nothing
        // to do yet. The ParticleCreateCommon post-hook will call this again once it has.
        return;
    }

    if (!grow_particle_emitter_manager_for_light_arrow_bank(emitterMng)) {
        mods::log::warn("failed to grow particle emitter manager resource array, Light Arrow "
                         "particles outside the horseback duel will be unavailable");
        return;
    }

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

    emitterMng->entryResourceManager(mgr, kLightArrowResMgrId);
    s_lightArrowResMgr = mgr;
    s_lightArrowJpcReady = true;

    mods::log::info("Pscene181.jpc loaded as particle bank {}, resource heap has {} bytes free",
                     kLightArrowResMgrId, heap->getFreeSize());
}

DEFINE_HOOK(&dPa_control_c::createCommon, ParticleCreateCommon);

static void on_particle_create_common_post(ModContext*, void*, void*, void*) {
    // createCommon() only ever runs once per process lifetime at boot, but reset defensively
    // in case it's ever re-entered, since mCommonResMng (and therefore our readiness check in
    // ensure_light_arrow_particles_loaded()) would otherwise look stale.
    s_lightArrowJpcReady = false;
    s_lightArrowResMgr = nullptr;

    if (s_lightArrowHeap != nullptr) {
        s_lightArrowHeap->destroy();
        s_lightArrowHeap = nullptr;
    }
    if (s_lightArrowHeapBacking != nullptr) {
        std::free(s_lightArrowHeapBacking);
        s_lightArrowHeapBacking = nullptr;
    }

    ensure_light_arrow_particles_loaded();
}

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
    return MOD_OK;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = mods::hook::add_pre<ActorDelete>(on_actor_delete_pre);
    if (result != MOD_OK) {
        mods::log::warn("failed to hook actor delete, deletion diagnostics will be unavailable: {}",
                         (int)result);
    }

    result = mods::hook::add_pre<HorseCallSubstance>(on_horse_call_substance_pre);
    if (result != MOD_OK) {
        mods::log::warn("failed to hook horse call, grass whistle may behave oddly: {}",
                         (int)result);
    }

    result = mods::hook::add_post<HoZeldaSetMatrix>(on_hozelda_set_matrix_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda matrix, orientation may be wrong during some cutscenes: {}",
            (int)result);
    }

    result = mods::hook::add_post<HoZeldaSetBowModel>(on_hozelda_set_bow_model_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda setBowModel, her bow will stay visible with auto-targeting "
            "disabled: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HoZeldaExecute>(on_hozelda_execute_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda execute, auto-targeting enemies will be unavailable: {}",
            (int)result);
    }

    result = mods::hook::add_post<HoZeldaExecute>(on_hozelda_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda execute post, Epona's reins may follow her hand while she's "
            "hidden in cutscenes: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HorseSetReinPosHandSubstance>(on_horse_set_rein_pos_hand_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook horse setReinPosHandSubstance, Epona's reins may keep following "
            "Zelda's hand while she draws and fires her bow: {}",
            (int)result);
    }

    result = mods::hook::replace<HoZeldaSetAnm>(on_hozelda_set_anm_replace);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda setAnm, auto-targeting enemies will be unavailable: {}",
            (int)result);
    }

    result = mods::hook::replace<HoZeldaSetNeckAngle>(on_hozelda_set_neck_angle_replace);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda setNeckAngle, Zelda will keep looking at Link instead of her "
            "bow target while riding solo: {}",
            (int)result);
    }

    result = mods::hook::add_pre<MidnaSearchNpc>(on_midna_search_npc_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Midna's NPC transform-block search, Zelda will act as a normal NPC "
            "and may prevent transforming near her: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HorseSearchEnemy>(on_horse_search_enemy_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's nearby-enemy search, she may rear up in fear while Zelda is "
            "riding solo and fighting nearby enemies: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HorseProcTurnInit>(on_horse_proc_turn_init_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's startled-turn reaction, she may rear up whenever a Light "
            "Arrow flies close to her while Zelda is riding solo and fighting: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HorseSetSingleAnime>(on_horse_set_single_anime_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's animation setter, she may still rear up or spook while "
            "Zelda is riding solo and fighting: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HorseSetNeckAnime>(on_horse_set_neck_anime_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's neck-animation setter, she may still toss her head in an "
            "alarmed reaction while Zelda is riding solo and fighting: {}",
            (int)result);
    }

    result = mods::hook::add_pre<HorseExecute>(on_horse_execute_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's execute() to clear her target-hit flags, she may still rear "
            "up whenever a Light Arrow flies close to her while Zelda is riding solo and "
            "fighting: {}",
            (int)result);
    }

    result = mods::hook::add_post<HorseExecute>(on_horse_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's execute() for the spooked-anim safety net: {}", (int)result);
    }

    result = mods::hook::add_post<ArrowShooting>(on_arrow_shooting_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook arrow shooting, auto-targeted Light Arrows may fly straight instead "
            "of toward their target: {}",
            (int)result);
    }

    result = mods::hook::add_post<ArrowExecute>(on_arrow_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook arrow execution, Zelda's Light Arrows may be invisible while the "
            "player is in first-person view and not drawing their own bow: {}",
            (int)result);
    }

    result = mods::hook::add_pre<CcAtCheck>(on_cc_at_check_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook attack-power check, Light Arrows won't one-hit-kill high-health "
            "enemies: {}",
            (int)result);
    }

    result = mods::hook::add_post<ParticleCreateCommon>(on_particle_create_common_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook particle common bank creation, Light Arrow particles outside the "
            "horseback duel will be unavailable: {}",
            (int)result);
    }

    result = mods::hook::replace<ParticleGetRmId>(on_particle_get_rm_id_replace);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook particle resource manager selection, Light Arrow particles outside "
            "the horseback duel will be unavailable: {}",
            (int)result);
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
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_mods_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    if (result != MOD_OK) {
        mods::log::warn("failed to register mods panel, the cutscene toggle won't be visible: {}",
                         (int)result);
    }

    // Normally this is a no-op here -- the particle system hasn't booted yet, so this will
    // actually load Pscene181.jpc once the ParticleCreateCommon post-hook above fires at the
    // end of the boot logo/title scene. But if this mod is enabled mid-session (well after
    // boot, with the particle system already up), that boot-time hook has already fired and
    // will never fire again, so this call is what loads the Light Arrow particles instead.
    ensure_light_arrow_particles_loaded();

    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daHorse_c* horse = dComIfGp_getHorseActor();

    if (!s_observedInitialHorseState) {
        s_observedInitialHorseState = true;
        // Nothing to defer if Epona isn't loaded yet -- the normal flow below only
        // ever creates/modifies her HoZelda actor once Epona herself has (re)appeared.
        // If she's already loaded on this very first tick, this mod was enabled
        // mid-session, so hold off until she goes through a real scene transition.
        s_safeToModifyEpona = (horse == nullptr);
    }

    if (!s_safeToModifyEpona) {
        if (horse != nullptr) {
            return MOD_OK;
        }
        // Epona just unloaded -- the next time she appears it will be through a
        // normal scene transition, so it's safe to resume making changes to her.
        s_safeToModifyEpona = true;
    }

    if (horse == nullptr) {
        remove_spawned_zelda();
        s_hideSpawnedZeldaInCutscene = false;
        daPy_py_c* link = daPy_getLinkPlayerActorClass();
        if (link != nullptr) {
            link->offHorseZelda();
        }
        return MOD_OK;
    }

    bool horseModelNotDrawn = horse->checkHorseCallWait();

    bool hadSpawnedZeldaBeforeRefresh = s_hasSpawnedZelda;
    s32 roomNo = dComIfGp_roomControl_getStayNo();
    fopAc_ac_c* trackedZelda = find_spawned_zelda();

    if (hadSpawnedZeldaBeforeRefresh && !s_hasSpawnedZelda) {
        s_zeldaLostInSameRoom = (roomNo == s_lastKnownZeldaRoomNo);
        if (kDiagnosticLoggingEnabled) {
            mods::log::warn(
                "spawned HoZelda actor id {} was deleted by the engine (not by this mod) while "
                "horseDemoMode={}, event='{}', room={}, layer={}, sameRoomAsBefore={}",
                (int)s_spawnedZeldaId, horse->checkHorseDemoMode(),
                dComIfGp_getEventManager().getRunEventName(), roomNo,
                dComIfG_play_c::getLayerNo(0), s_zeldaLostInSameRoom);
        }
    }

    if (s_hasSpawnedZelda) {
        s_lastKnownZeldaRoomNo = roomNo;
    }

    fopAc_ac_c* attachedZelda = horse->getZeldaActor();
    if (kDiagnosticLoggingEnabled && trackedZelda != nullptr && attachedZelda != nullptr &&
        attachedZelda != trackedZelda) {
        mods::log::warn(
            "horse has some other HoZelda actor ({}) attached, but our own HoZelda actor id {} "
            "is still alive and unattached -- two Zelda actors may be visible at once",
            (void*)attachedZelda, (int)s_spawnedZeldaId);
    }

    bool inScriptedCutscene = horse->checkHorseDemoMode() && !is_title_screen();

    bool storyHoZeldaActive = find_other_hozelda() != nullptr;

    if (storyHoZeldaActive) {
        remove_spawned_zelda();
        s_hideSpawnedZeldaInCutscene = false;
    } else if (inScriptedCutscene) {
        s_hideSpawnedZeldaInCutscene = !show_zelda_in_cutscenes() || is_always_hidden_cutscene();

        bool isDemo01_03 =
            names_equal_case_insensitive(dComIfGp_getEventManager().getRunEventName(), "demo01_03");
        if (horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda && s_zeldaLostInSameRoom &&
            isDemo01_03) {
            spawn_zelda_on_horse(horse);
        }
    } else if (horseModelNotDrawn) {
        s_hideSpawnedZeldaInCutscene = true;
        if (horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda) {
            spawn_zelda_on_horse(horse);
        }
    } else if (horse->getZeldaActor() == nullptr && !s_hasSpawnedZelda) {
        s_hideSpawnedZeldaInCutscene = false;
        spawn_zelda_on_horse(horse);
    } else {
        s_hideSpawnedZeldaInCutscene = false;
    }

    log_cutscene_diagnostics_if_changed(horse, is_title_screen(), inScriptedCutscene,
                                         storyHoZeldaActive);

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    remove_spawned_zelda();
    mods::hook::uninstall<ActorDelete>();
    mods::hook::uninstall<HorseCallSubstance>();
    mods::hook::uninstall<HoZeldaSetMatrix>();
    mods::hook::uninstall<HoZeldaSetBowModel>();
    mods::hook::uninstall<HoZeldaExecute>();
    mods::hook::uninstall<HorseSetReinPosHandSubstance>();
    mods::hook::uninstall<HoZeldaSetAnm>();
    mods::hook::uninstall<HoZeldaSetNeckAngle>();
    mods::hook::uninstall<ArrowShooting>();
    mods::hook::uninstall<ArrowExecute>();
    return MOD_OK;
}
}
