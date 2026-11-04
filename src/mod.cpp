#include <cctype>
#include <cstdlib>
#include <cstring>

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/flow.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_b_gnd.h"
#include "d/actor/d_a_e_sh.h"
#include "d/actor/d_a_horse.h"
#include "d/actor/d_a_hozelda.h"
#include "d/actor/d_a_npc.h"
#include "d/actor/d_a_npc_zelda.h"
#include "d/actor/d_a_player.h"
#include "d/d_attention.h"
#include "d/d_cc_d.h"
#include "d/d_cc_uty.h"
#include "d/d_com_inf_game.h"
#include "d/d_event_manager.h"
#include "d/d_msg_class.h"
#include "d/d_msg_flow.h"
#include "d/d_msg_object.h"
#include "d/d_particle.h"
#include "d/d_stage.h"
#include "SSystem/SComponent/c_math.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "helpers/string.hpp"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/JKernel/JKRDvdRipper.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JParticle/JPAResourceManager.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_lib.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2SeMgr.h"

DEFINE_MOD();

IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(FlowService, svc_flow);
IMPORT_SERVICE(MessageService, svc_message);

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

static ConfigVarHandle g_cvarEponaFollowDismounted = 0;

static bool epona_follow_dismounted_enabled() {
    bool value = false;
    if (g_cvarEponaFollowDismounted == 0 ||
        svc_config->get_bool(mod_ctx, g_cvarEponaFollowDismounted, &value) != MOD_OK) {
        return false;
    }
    return value;
}

static bool epona_follow_active() {
    return auto_target_enemies_enabled() && epona_follow_dismounted_enabled();
}

static constexpr bool kDiagnosticLoggingEnabled = false;

static const char* kHoZeldaStageName = "HoZelda";

static ActorId s_spawnedZeldaId = 0;
static bool s_hasSpawnedZelda = false;

static s32 s_lastKnownZeldaRoomNo = -1;
static bool s_zeldaLostInSameRoom = false;

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

static bool is_spawned_zelda_riding_solo() {
    fopAc_ac_c* actor = find_spawned_zelda();
    return actor != nullptr && static_cast<daHoZelda_c*>(actor)->checkSingleRide();
}

static bool s_hideSpawnedZeldaInCutscene = false;

static bool s_zeldaBackwardBowTarget = false;

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
        "Demo19_02",
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

    if (dualRide && zelda->mBowMode != 0 && s_zeldaBackwardBowTarget) {
        s_turnStandBlend = 0.0f;
        zelda->shape_angle.y = (s16)(zelda->shape_angle.y + 0x8000);
        zelda->current.angle.y = zelda->shape_angle.y;
        mDoMtx_stack_c::transS(zelda->current.pos);
        mDoMtx_stack_c::ZXYrotM(zelda->shape_angle.x, zelda->shape_angle.y, zelda->shape_angle.z);
        zelda->model->setBaseTRMtx(mDoMtx_stack_c::get());
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
    zelda->mpBowModel->calc();
}

static void on_hozelda_set_bow_model_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda) || zelda->mpBowModel == nullptr) {
        return;
    }

    if (!s_hideSpawnedZeldaInCutscene && !auto_target_enemies_enabled()) {
        hide_bow_visually(zelda);
    }
}

static const f32 kAutoTargetRange = 4000.0f;

struct HoZeldaTargetSearch {
    const fopAc_ac_c* self;
    f32 maxDistSq;
    s16 maxAngle;
    s16 refAngleY;
    bool behind;
    fopAc_ac_c* best;
    f32 bestDistSq;
};

static s32 mirror_seen_angle_behind(s32 seenAngleY) {
    return 0x8000 - seenAngleY;
}

static s32 seen_angle_from_yaw(const cXyz& i_fromPos, const cXyz& i_targetPos, s16 i_refAngleY) {
    s16 targetAngle = cLib_targetAngleY(&i_fromPos, &i_targetPos);
    targetAngle -= i_refAngleY;
    return abs(targetAngle);
}

static s16 zelda_targeting_reference_angle_y(const daHoZelda_c* zelda) {
    daHorse_c* horse = dComIfGp_getHorseActor();
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    bool dualRide = horse != nullptr && player != nullptr && player->checkHorseRide();
    return dualRide ? horse->shape_angle.y : zelda->shape_angle.y;
}

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
    s32 seenAngleY =
        seen_angle_from_yaw(search->self->current.pos, i_actor->current.pos, search->refAngleY);
    if (search->behind) {
        seenAngleY = mirror_seen_angle_behind(seenAngleY);
    }

    if (distSq > search->maxDistSq || seenAngleY > search->maxAngle) {
        return NULL;
    }

    if (search->best == NULL || distSq < search->bestDistSq) {
        search->best = i_actor;
        search->bestDistSq = distSq;
    }

    return NULL;
}

static fopAc_ac_c* find_nearest_enemy(daHoZelda_c* zelda, bool i_behind) {
    HoZeldaTargetSearch search = {};
    search.self = zelda;
    search.maxDistSq = kAutoTargetRange * kAutoTargetRange;
    search.maxAngle = zelda->mpHIO->m.bow_end_angle;
    search.behind = i_behind;
    search.refAngleY = zelda_targeting_reference_angle_y(zelda);
    fopAcIt_Judge((fopAcIt_JudgeFunc)judge_nearest_enemy, &search);
    return search.best;
}

DEFINE_HOOK(&daHoZelda_c::execute, HoZeldaExecute);

static HookAction on_hozelda_execute_pre(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda)) {
        return HOOK_CONTINUE;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    bool inCutscene = horse != nullptr && horse->checkHorseDemoMode() && !is_title_screen();

    if (auto_target_enemies_enabled() && !inCutscene) {
        fopAc_ac_c* target = find_nearest_enemy(zelda, false);
        bool backward = false;

        daPy_py_c* player = daPy_getLinkPlayerActorClass();
        bool dualRide = player != nullptr && player->checkHorseRide();
        if (target == nullptr && dualRide) {
            target = find_nearest_enemy(zelda, true);
            backward = target != nullptr;
        }

        zelda->mGndAcKeep.setData(target);
        s_zeldaBackwardBowTarget = backward;
    } else {
        zelda->mGndAcKeep.clearData();
        s_zeldaBackwardBowTarget = false;
    }
    return HOOK_CONTINUE;
}

static void force_reins_to_normal_pos(daHoZelda_c* zelda, daHorse_c* horse) {
    if (horse->getZeldaActor() == static_cast<fopAc_ac_c*>(zelda)) {
        horse->setZeldaActor(nullptr);
    }
    horse->setReinPosNormal();
}

static fopAc_ac_c* find_zelda_npc_proxy();
static void sync_zelda_npc_proxy_from_companion(daNpc_Zelda_c* proxy, fopAc_ac_c* companion);

static void on_hozelda_execute_post(ModContext*, void* args, void*, void*) {
    daHoZelda_c* zelda = mods::arg<daHoZelda_c*>(args, 0);
    if (!is_spawned_zelda(zelda)) {
        return;
    }

    fopAc_ac_c* proxy = find_zelda_npc_proxy();
    if (proxy != nullptr) {
        sync_zelda_npc_proxy_from_companion(static_cast<daNpc_Zelda_c*>(proxy), zelda);
    }

    if (!s_hideSpawnedZeldaInCutscene) {
        return;
    }

    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr) {
        return;
    }

    force_reins_to_normal_pos(zelda, horse);
}

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

static bool epona_combat_reactions_suppressed(daHorse_c* horse) {
    (void)horse;
    return auto_target_enemies_enabled() && is_spawned_zelda_riding_solo();
}

static bool is_dismounted_player_tg_hit(daHorse_c* horse) {
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    bool dismounted = player != nullptr && !player->checkHorseRide();
    fopAc_ac_c* playerActor = dismounted ? static_cast<fopAc_ac_c*>(player) : nullptr;

    bool result = dismounted &&
                  ((horse->m_tgco_cyl[0].ChkTgHit() && horse->m_tgco_cyl[0].GetTgHitAc() == playerActor) ||
                      (horse->m_tgco_cyl[1].ChkTgHit() && horse->m_tgco_cyl[1].GetTgHitAc() == playerActor) ||
                      (horse->m_tgco_cyl[2].ChkTgHit() && horse->m_tgco_cyl[2].GetTgHitAc() == playerActor) ||
                      (horse->m_head_sph.ChkTgHit() && horse->m_head_sph.GetTgHitAc() == playerActor));

    return result;
}

static bool s_allowRearForDismountedPlayerHit = false;

static constexpr u16 kAnmHorseStand = 20;
static constexpr u16 kAnmHorseExcitement = 8;
static constexpr u16 kAnmHorseExcitement2 = 9;
static constexpr u16 kAnmHorseWait01 = 27;
static constexpr u16 kAnmHorseLash = 14;

static void set_anm_auto_target(daHoZelda_c* zelda) {
    u16 anm_idx[3];
    daHorse_c* horse = (daHorse_c*)dComIfGp_getHorseActor();
    daPy_py_c* player = daPy_getLinkPlayerActorClass();

    if (horse == NULL) {
        return;
    }

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
        } else if (horse_anm_idx == 0x11) {
            *anm_p = 0xF;
        } else if (horse_anm_idx == 0x12 || horse_anm_idx == 0x13) {
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
        target_seen_angleY = seen_angle_from_yaw(zelda->current.pos, target_actor->current.pos,
                                                  zelda_targeting_reference_angle_y(zelda));
    }

    int bow_mode_angleY =
        s_zeldaBackwardBowTarget ? mirror_seen_angle_behind(target_seen_angleY) : target_seen_angleY;

    bool target_vulnerable = false;
    if (ganondorf != NULL) {
        target_vulnerable = ganondorf->checkPiyo() != 1;
    } else if (target_actor != NULL) {
        target_vulnerable = target_actor->health > 0;
    }

    if ((anm_idx[0] == 0xE || anm_idx[0] == 0x1C) && zelda->field_0x6da == 0 && !zelda->mDamageInit &&
        zelda->field_0x6dd == 0 && target_actor != NULL && target_vulnerable &&
        (bow_mode_angleY < zelda->mpHIO->m.bow_start_angle ||
            (zelda->mBowMode != 0 && bow_mode_angleY < zelda->mpHIO->m.bow_end_angle)))
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

DEFINE_HOOK_SYMBOL("daMidna_searchNpc", void*(fopAc_ac_c*, void*), MidnaSearchNpc);

static HookAction on_midna_search_npc_pre(ModContext*, void* args, void*, void*) {
    fopAc_ac_c* actor = mods::arg<fopAc_ac_c*>(args, 0);
    if (is_spawned_zelda(actor)) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK_SYMBOL("daHorse_searchEnemy", void*(fopAc_ac_c*, void*), HorseSearchEnemy);

static HookAction on_horse_search_enemy_pre(ModContext*, void*, void*, void*) {
    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse != nullptr && epona_combat_reactions_suppressed(horse)) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daHorse_c::procTurnInit, HorseProcTurnInit);

static HookAction on_horse_proc_turn_init_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    bool suppressed = epona_combat_reactions_suppressed(horse);

    if (suppressed && !s_allowRearForDismountedPlayerHit) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daHorse_c::setSingleAnime, HorseSetSingleAnime);

static HookAction on_horse_set_single_anime_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    u16 anmIdx = mods::arg<u16>(args, 1);
    bool isSpookedAnime = anmIdx == kAnmHorseStand || anmIdx == kAnmHorseExcitement ||
                           anmIdx == kAnmHorseExcitement2;
    if (isSpookedAnime) {
        bool suppressed = epona_combat_reactions_suppressed(horse);
        if (suppressed && !s_allowRearForDismountedPlayerHit) {
            return HOOK_SKIP_ORIGINAL;
        }
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daHorse_c::setNeckAnime, HorseSetNeckAnime);

static HookAction on_horse_set_neck_anime_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    u16 anmIdx = mods::arg<u16>(args, 1);
    if (anmIdx != kAnmHorseLash) {
        return HOOK_CONTINUE;
    }

    bool suppressed = epona_combat_reactions_suppressed(horse);
    if (suppressed && !s_allowRearForDismountedPlayerHit) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daArrow_c::arrowShooting, ArrowShooting);

DEFINE_HOOK(&daHorse_c::execute, HorseExecute);

static HookAction on_horse_execute_pre(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    if (!epona_combat_reactions_suppressed(horse)) {
        return HOOK_CONTINUE;
    }

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

static void on_horse_execute_post(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);

    u16 anmIdx = horse->getAnmIdx(0);
    bool isSpookedAnime = anmIdx == kAnmHorseStand || anmIdx == kAnmHorseExcitement ||
                          anmIdx == kAnmHorseExcitement2;

    bool inTurnState = horse->m_procID == daHorse_c::PROC_TURN_e;

    if (!isSpookedAnime && !inTurnState) {
        s_allowRearForDismountedPlayerHit = false;
        return;
    }

    bool suppressed = epona_combat_reactions_suppressed(horse);
    if (suppressed && !s_allowRearForDismountedPlayerHit) {
        horse->setSingleAnime(kAnmHorseWait01, 1.0f, 0.0f, -1, 0.0f, 0);
        horse->procWaitInit();
    }
}

static constexpr f32 kEponaFollowDistance = 1000.0f;
static constexpr f32 kEponaFollowRampRange = 300.0f;

static constexpr f32 kEponaFollowMaxStickValue = 0.8f;

static constexpr f32 kEponaFollowMaxDistance = 5000.0f;

static constexpr f32 kEponaFollowResumeMargin = 60.0f;

static constexpr f32 kEponaFollowStillDistance = 650.0f;

static bool s_eponaFollowApproaching = true;

static bool s_eponaFollowFacing = true;

static constexpr s16 kEponaFollowIdleTurnMax = 0x600;
static constexpr s16 kEponaFollowIdleTurnMin = 0x40;

static constexpr s16 kEponaFollowAlignmentCone = 0x3000;

static bool epona_follow_should_engage(daHorse_c* horse, daPy_py_c* player) {
    if (horse == nullptr || player == nullptr || !epona_follow_active()) {
        s_eponaFollowApproaching = true;
        s_eponaFollowFacing = true;
        return false;
    }
    if (player->checkHorseRide()) {
        s_eponaFollowApproaching = true;
        s_eponaFollowFacing = true;
        return false;
    }
    if (horse->checkHorseDemoMode() || horse->checkHorseCallWait() || dComIfGp_event_runCheck()) {
        return false;
    }
    if (horse->checkRodeoMode() || horse->checkStateFlg0(daHorse_c::FLG0_UNK_10000000)) {
        return false;
    }
    if (horse->current.pos.absXZ(player->current.pos) > kEponaFollowMaxDistance) {
        s_eponaFollowApproaching = true;
        s_eponaFollowFacing = true;
        return false;
    }
    return true;
}

DEFINE_HOOK(&daHorse_c::setStickData, HorseSetStickData);

static void on_horse_set_stick_data_post(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    if (!epona_follow_should_engage(horse, player)) {
        return;
    }

    s16 bearing = cLib_targetAngleY(&horse->current.pos, &player->current.pos);
    horse->m_padStickAngleY = bearing;

    f32 distXZ = horse->current.pos.absXZ(player->current.pos);
    if (s_eponaFollowApproaching) {
        if (distXZ <= kEponaFollowDistance) {
            s_eponaFollowApproaching = false;
        }
    } else if (distXZ > kEponaFollowDistance + kEponaFollowResumeMargin) {
        s_eponaFollowApproaching = true;
    }

    if (s_eponaFollowFacing) {
        if (distXZ <= kEponaFollowStillDistance) {
            s_eponaFollowFacing = false;
        }
    } else if (distXZ > kEponaFollowStillDistance + kEponaFollowResumeMargin) {
        s_eponaFollowFacing = true;
    }

    s16 facingError = bearing - horse->shape_angle.y;
    bool aligned = (facingError <= kEponaFollowAlignmentCone) &&
                   (facingError >= -kEponaFollowAlignmentCone);
    if (!s_eponaFollowApproaching || !aligned) {
        horse->m_padStickValue = 0.0f;
        return;
    }

    f32 magnitude = (distXZ - kEponaFollowDistance) / kEponaFollowRampRange;
    horse->m_padStickValue = magnitude > kEponaFollowMaxStickValue ? kEponaFollowMaxStickValue : magnitude;

    horse->onMoveAccept();
}

static void on_horse_follow_execute_post(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    if (!epona_follow_should_engage(horse, player)) {
        return;
    }

    if (!s_eponaFollowFacing) {
        return;
    }

    s16 bearing = cLib_targetAngleY(&horse->current.pos, &player->current.pos);
    s16 facingError = bearing - horse->shape_angle.y;
    bool aligned = (facingError <= kEponaFollowAlignmentCone) &&
                   (facingError >= -kEponaFollowAlignmentCone);
    if (s_eponaFollowApproaching && aligned) {
        return;
    }

    cLib_addCalcAngleS(&horse->shape_angle.y, bearing, 5, kEponaFollowIdleTurnMax,
                        kEponaFollowIdleTurnMin);
    horse->current.angle.y = horse->shape_angle.y;
    horse->initHorseMtx();
}

static bool zelda_talk_event_active() {
    fopAc_ac_c* proxy = find_zelda_npc_proxy();
    return proxy != nullptr && dComIfGp_event_runCheck() && dComIfGp_getEvent()->getPt2() == proxy;
}

static constexpr s16 kZeldaTalkHeadTrackCone = static_cast<s16>(0x8000 * 50 / 180);

static constexpr s16 kZeldaTalkFaceTurnMax = 0x800;
static constexpr s16 kZeldaTalkFaceTurnMin = 0x40;

static void on_horse_talk_face_execute_post(ModContext*, void* args, void*, void*) {
    daHorse_c* horse = mods::arg<daHorse_c*>(args, 0);
    daPy_py_c* player = daPy_getLinkPlayerActorClass();
    if (horse == nullptr || player == nullptr || !zelda_talk_event_active()) {
        return;
    }

    s16 bearingToPlayer = cLib_targetAngleY(&horse->current.pos, &player->current.pos);
    s16 diff = bearingToPlayer - horse->shape_angle.y;
    if (diff > kZeldaTalkHeadTrackCone) {
        diff = kZeldaTalkHeadTrackCone;
    } else if (diff < -kZeldaTalkHeadTrackCone) {
        diff = -kZeldaTalkHeadTrackCone;
    }
    s16 target = bearingToPlayer - diff;

    cLib_addCalcAngleS(&horse->shape_angle.y, target, 5, kZeldaTalkFaceTurnMax,
                        kZeldaTalkFaceTurnMin);
    horse->current.angle.y = horse->shape_angle.y;
    horse->initHorseMtx();
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

static void ensure_light_arrow_particles_loaded() {
    if (s_lightArrowJpcReady) {
        return;
    }

    dPa_control_c* control = g_dComIfG_gameInfo.play.getParticle();
    JPAEmitterManager* emitterMng = dPa_control_c::getEmitterManager();
    if (control == nullptr || control->mCommonResMng == nullptr || emitterMng == nullptr) {
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

static constexpr u16 kZeldaTalkMessageGroup = 0;

static constexpr u16 kZeldaTalkFlowId = 0x7000;

static mods::flow::Graph s_zeldaTalkGraph;
static std::vector<mods::flow::RegisteredMessage> s_zeldaTalkMessages;
static u16 s_zeldaTalkNodeId = 0;

static u16 s_zeldaTalkWolfNodeId = 0;

static MessageId s_zeldaTalkPage1MessageId = 0;

static constexpr u16 kZeldaTalkWolfPauseFrames = 30;

static constexpr u16 kZeldaTalkHealEffectDelayFrames = 90;

static constexpr s32 kZeldaTalkHealEffectDurationFrames = 90;

static constexpr f32 kZeldaTalkHealAmount = 32.0f;

static constexpr f32 kZeldaTalkHealEffectStartHeight = 0.0f;

static constexpr f32 kZeldaTalkHealEffectEndHeightAboveHead = 20.0f;

static constexpr f32 kZeldaTalkHealEffectStartRadius = 140.0f;
static constexpr f32 kZeldaTalkHealEffectEndRadius = 10.0f;

static constexpr s16 kZeldaTalkHealEffectAngularVelocity = -0x1CB8;

static constexpr f32 kZeldaTalkHealEffectSpinRampFrames = 25.0f;

static u32 s_zeldaTalkHealGlow0EmitterId = 0;
static u32 s_zeldaTalkHealGlow1EmitterId = 0;
static u32 s_zeldaTalkHealStarEmitterId = 0;
static s32 s_zeldaTalkHealEffectFramesLeft = -1;
static s16 s_zeldaTalkHealEffectAngle = 0;

static void stop_zelda_talk_heal_effect() {
    if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(s_zeldaTalkHealGlow0EmitterId)) {
        emitter->stopDrawParticle();
    }
    if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(s_zeldaTalkHealGlow1EmitterId)) {
        emitter->stopDrawParticle();
    }
    if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(s_zeldaTalkHealStarEmitterId)) {
        emitter->stopDrawParticle();
    }
    Z2GetAudioMgr()->seStop(Z2SE_FAIRY_S_LV, 0);
    s_zeldaTalkHealEffectFramesLeft = -1;
}

static void refresh_zelda_talk_heal_effect(daPy_py_c* player) {
    s32 elapsed = kZeldaTalkHealEffectDurationFrames - s_zeldaTalkHealEffectFramesLeft;
    f32 spinScale = (f32)elapsed / kZeldaTalkHealEffectSpinRampFrames;
    if (spinScale > 1.0f) {
        spinScale = 1.0f;
    }
    s_zeldaTalkHealEffectAngle += (s16)(kZeldaTalkHealEffectAngularVelocity * spinScale);

    f32 progress = (f32)s_zeldaTalkHealEffectFramesLeft / (f32)kZeldaTalkHealEffectDurationFrames;
    f32 radius = kZeldaTalkHealEffectEndRadius +
                 (kZeldaTalkHealEffectStartRadius - kZeldaTalkHealEffectEndRadius) * progress;

    f32 elapsedFrac = (f32)elapsed / (f32)kZeldaTalkHealEffectDurationFrames;
    if (elapsedFrac > 1.0f) {
        elapsedFrac = 1.0f;
    }
    f32 endHeight =
        (player->getHeadTopPos().y - player->current.pos.y) + kZeldaTalkHealEffectEndHeightAboveHead;
    f32 height = kZeldaTalkHealEffectStartHeight +
                 (endHeight - kZeldaTalkHealEffectStartHeight) * elapsedFrac;

    cXyz offset(radius * cM_ssin(s_zeldaTalkHealEffectAngle), height,
                radius * cM_scos(s_zeldaTalkHealEffectAngle));
    cXyz pos = player->current.pos + offset;

    s_zeldaTalkHealGlow0EmitterId = dComIfGp_particle_set(
        s_zeldaTalkHealGlow0EmitterId, ID_ZF_J_FAIRY00_GLOW, &pos, nullptr, nullptr);
    s_zeldaTalkHealGlow1EmitterId = dComIfGp_particle_set(
        s_zeldaTalkHealGlow1EmitterId, ID_ZF_J_FAIRY01_GLOW, &pos, nullptr, nullptr);
    s_zeldaTalkHealStarEmitterId = dComIfGp_particle_set(
        s_zeldaTalkHealStarEmitterId, ID_ZF_J_FAIRY03_STAR, &pos, nullptr, nullptr);

    Z2GetAudioMgr()->seStartLevel(Z2SE_FAIRY_S_LV, &pos, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
}

static void trigger_zelda_talk_heal_effect(daPy_py_c* player) {
    dComIfGp_setItemLifeCount(kZeldaTalkHealAmount, 0);
    s_zeldaTalkHealEffectAngle = 0;
    s_zeldaTalkHealEffectFramesLeft = kZeldaTalkHealEffectDurationFrames;
    refresh_zelda_talk_heal_effect(player);
    Z2GetAudioMgr()->seStart(Z2SE_FAIRY_S_FLY_RTT, &player->current.pos, 0, 0, 1.0f, 1.0f, -1.0f,
                             -1.0f, 0);
}

static constexpr uint8_t kZeldaTalkSpeakerId = 5;

static ActorId s_zeldaNpcProxyId = 0;
static bool s_hasZeldaNpcProxy = false;

static fopAc_ac_c* find_zelda_npc_proxy() {
    if (!s_hasZeldaNpcProxy) {
        return nullptr;
    }

    if (fpcM_IsCreating((fpc_ProcID)s_zeldaNpcProxyId)) {
        return nullptr;
    }

    fopAc_ac_c* actor = nullptr;
    fopAcM_SearchByID((fpc_ProcID)s_zeldaNpcProxyId, &actor);
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_NPC_ZELDA_e) {
        s_hasZeldaNpcProxy = false;
        return nullptr;
    }
    return actor;
}

static bool is_zelda_npc_proxy(const fopAc_ac_c* actor) {
    return s_hasZeldaNpcProxy && actor != nullptr &&
           (ActorId)fopAcM_GetID(actor) == s_zeldaNpcProxyId;
}

static void spawn_zelda_npc_proxy(fopAc_ac_c* companion) {
    ActorSpawnParams params{};
    params.parameters = 0;
    params.argument = 0;
    params.room_num = fopAcM_GetRoomNo(companion);
    params.position = {companion->current.pos.x, companion->current.pos.y,
                        companion->current.pos.z};
    params.angle = {(int16_t)kZeldaTalkFlowId, companion->shape_angle.y, 0};
    params.scale = {1.0f, 1.0f, 1.0f};

    ActorId newId = 0;
    ModResult result = svc_actor->create_actor(mod_ctx, fpcNm_NPC_ZELDA_e, &params, &newId);
    if (result != MOD_OK) {
        mods::log::warn("failed to spawn Zelda NPC talk proxy: {}", (int)result);
        return;
    }

    s_zeldaNpcProxyId = newId;
    s_hasZeldaNpcProxy = true;
}

static void remove_zelda_npc_proxy() {
    if (!s_hasZeldaNpcProxy) {
        return;
    }

    svc_actor->delete_actor(mod_ctx, s_zeldaNpcProxyId);
    s_hasZeldaNpcProxy = false;
}

static ModResult register_zelda_talk_flow() {
    static constexpr std::array kAllLanguages{
        MESSAGE_LANGUAGE_ENGLISH,
        MESSAGE_LANGUAGE_GERMAN,
        MESSAGE_LANGUAGE_FRENCH,
        MESSAGE_LANGUAGE_SPANISH,
        MESSAGE_LANGUAGE_ITALIAN,
        MESSAGE_LANGUAGE_JAPANESE,
    };

    auto register_page = [&](auto&& i_configure, MessageId& o_id) -> ModResult {
        std::vector<mods::flow::MessageVariant> variants;
        variants.reserve(kAllLanguages.size());
        for (MessageLanguage language : kAllLanguages) {
            mods::flow::MessageBuilder builder{mods::flow::MessageStyle{}
                                                    .box_kind(MESSAGE_BOX_TALK)
                                                    .speaker(kZeldaTalkSpeakerId)};
            i_configure(builder);
            variants.push_back(builder.build(language));
        }

        auto message = mods::flow::register_message(kZeldaTalkMessageGroup, variants);
        if (!message) {
            return message.result();
        }
        o_id = message.id();
        s_zeldaTalkMessages.push_back(std::move(message));
        return MOD_OK;
    };

    MessageId page1Id = 0;
    MessageId page2Id = 0;
    ModResult result = register_page(
        [](mods::flow::MessageBuilder& builder) {
            builder.text("Greetings, ")
                .player_name()
                .text("!\nI pray for your continued safety.")
                .input_after_delay(kZeldaTalkHealEffectDelayFrames);
        },
        page1Id);
    if (result == MOD_OK) {
        result = register_page(
            [](mods::flow::MessageBuilder& builder) {
                builder.raw_tag(0, MSGTAG_HORSE_NAME, {})
                    .text(" and I are ready to depart\nwhenever you are.");
            },
            page2Id);
    }
    if (result != MOD_OK) {
        return result;
    }
    s_zeldaTalkPage1MessageId = page1Id;

    MessageId wolfPage1Id = 0;
    MessageId wolfPage2Id = 0;
    result = register_page(
        [](mods::flow::MessageBuilder& builder) {
            builder.text("Midna.")
                .pause(kZeldaTalkWolfPauseFrames)
                .text(" Do you really have to ride\nour hero around like a horse?");
        },
        wolfPage1Id);
    if (result == MOD_OK) {
        result = register_page(
            [](mods::flow::MessageBuilder& builder) {
                builder.pause(kZeldaTalkWolfPauseFrames)
                    .raw_tag(0, MSGTAG_INSTANT, {})
                    .text("So undignified...");
            },
            wolfPage2Id);
    }
    if (result != MOD_OK) {
        return result;
    }

    mods::flow::GraphBuilder graph{kZeldaTalkMessageGroup};
    const auto page2 = graph.add_message(page2Id).next(mods::flow::kEnd);
    const auto page1 = graph.add_message(page1Id).next(page2);
    s_zeldaTalkNodeId = page1;

    const auto wolfPage2 = graph.add_message(wolfPage2Id).next(mods::flow::kEnd);
    const auto wolfPage1 = graph.add_message(wolfPage1Id).next(wolfPage2);
    s_zeldaTalkWolfNodeId = wolfPage1;

    s_zeldaTalkGraph = graph.commit();
    return s_zeldaTalkGraph.result();
}

static HookAction on_npc_zelda_draw_pre(ModContext*, void* args, void*, void*) {
    daNpc_Zelda_c* actor = mods::arg<daNpc_Zelda_c*>(args, 0);
    if (is_zelda_npc_proxy(actor)) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daNpc_Zelda_c::Draw, NpcZeldaDraw);

static HookAction on_npc_zelda_set_collision_pre(ModContext*, void* args, void*, void*) {
    daNpc_Zelda_c* actor = mods::arg<daNpc_Zelda_c*>(args, 0);
    if (is_zelda_npc_proxy(actor)) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daNpc_Zelda_c::setCollision, NpcZeldaSetCollision);

static bool zelda_talk_may_be_targeted(fopAc_ac_c* zelda, daPy_py_c* player) {
    if (zelda == nullptr || player == nullptr || player->checkHorseRide()) {
        return false;
    }
    return !s_hideSpawnedZeldaInCutscene && !dComIfGp_event_runCheck();
}

static void sync_zelda_npc_proxy_from_companion(daNpc_Zelda_c* proxy, fopAc_ac_c* companion) {
    proxy->current.pos = companion->current.pos;
    proxy->current.angle.y = companion->shape_angle.y;
    proxy->shape_angle.y = companion->shape_angle.y;
    proxy->mCurAngle.y = companion->shape_angle.y;
    fopAcM_SetRoomNo(proxy, fopAcM_GetRoomNo(companion));

    proxy->attention_info.position = companion->attention_info.position;

    proxy->attention_info.distances[fopAc_attn_TALK_e] = 18;
}

static void on_npc_zelda_execute_post(ModContext*, void* args, void*, void*) {
    daNpc_Zelda_c* proxy = mods::arg<daNpc_Zelda_c*>(args, 0);
    if (!is_zelda_npc_proxy(proxy)) {
        return;
    }

    fopAc_ac_c* companion = find_spawned_zelda();
    daPy_py_c* player = daPy_getLinkPlayerActorClass();

    if (companion != nullptr) {
        sync_zelda_npc_proxy_from_companion(proxy, companion);
    }

    proxy->attention_info.flags &= ~fopAc_AttnFlag_SPEAK_e;
    bool eligible = companion != nullptr && zelda_talk_may_be_targeted(companion, player);
    if (eligible) {
        proxy->attention_info.flags |= fopAc_AttnFlag_TALK_e;
    } else {
        proxy->attention_info.flags &= ~fopAc_AttnFlag_TALK_e;
    }

    if (eligible) {
        proxy->eventInfo.onCondition(dEvtCnd_CANTALK_e);
    }
}

DEFINE_HOOK(&daNpc_Zelda_c::Execute, NpcZeldaExecute);

static HookAction on_msg_flow_get_init_node_index_pre(ModContext*, void* args, void* retval, void*) {
    dMsgFlow_c* self = mods::arg<dMsgFlow_c*>(args, 0);
    u16 flowId = mods::arg<u16>(args, 1);
    fopAc_ac_c* proxy = find_zelda_npc_proxy();
    if (proxy != nullptr && flowId == kZeldaTalkFlowId &&
        self == &static_cast<daNpc_Zelda_c*>(proxy)->mFlow) {
        daPy_py_c* player = daPy_getLinkPlayerActorClass();
        bool isWolf = player != nullptr && player->checkWolf() != 0;
        u16 startNode = isWolf ? s_zeldaTalkWolfNodeId : s_zeldaTalkNodeId;
        if (retval != nullptr) {
            *static_cast<u16*>(retval) = startNode;
        }
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

DEFINE_HOOK(&dMsgFlow_c::getInitNodeIndex, MsgFlowGetInitNodeIndex);

static void sync_zelda_npc_proxy_lifecycle(fopAc_ac_c* companion) {
    find_zelda_npc_proxy();
    if (companion == nullptr) {
        remove_zelda_npc_proxy();
        return;
    }
    if (!s_hasZeldaNpcProxy) {
        spawn_zelda_npc_proxy(companion);
    }
}

static void update_zelda_talk_heal_effect() {
    if (s_zeldaTalkHealEffectFramesLeft >= 0) {
        if (daPy_py_c* player = daPy_getLinkPlayerActorClass()) {
            refresh_zelda_talk_heal_effect(player);
        }
        if (--s_zeldaTalkHealEffectFramesLeft < 0) {
            stop_zelda_talk_heal_effect();
        }
    }

    static bool s_wasTyping = false;
    static bool s_effectFiredThisPage = false;

    msg_class* msg_p = dMsgObject_c::getActor();
    bool onPage1 = zelda_talk_event_active() && msg_p != nullptr &&
                   msg_p->msg_idx == s_zeldaTalkPage1MessageId;
    if (!onPage1) {
        s_wasTyping = false;
        s_effectFiredThisPage = false;
        return;
    }

    bool isTyping = msg_p->mode == fopMsg_MODE_MSG_TYPING_e;
    if (s_wasTyping && !isTyping && !s_effectFiredThisPage) {
        if (daPy_py_c* player = daPy_getLinkPlayerActorClass()) {
            trigger_zelda_talk_heal_effect(player);
        }
        s_effectFiredThisPage = true;
    }
    s_wasTyping = isTyping;
}

static void update_zelda_talk_interaction() {
    fopAc_ac_c* companion = find_spawned_zelda();
    sync_zelda_npc_proxy_lifecycle(companion);
    update_zelda_talk_heal_effect();
}

static bool is_epona_follow_control_disabled(ModContext*, void*) {
    return !auto_target_enemies_enabled();
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
                                  "Ganondorf during the horseback duel. While sharing the saddle "
                                  "with Link, she can also aim and fire backwards at enemies "
                                  "behind her if none are ahead. When off (default), she just "
                                  "rides along passively.";
    autoTargetControl.binding = UI_BINDING_CONFIG_VAR;
    autoTargetControl.config_var = g_cvarAutoTargetEnemies;
    svc_ui->pane_add_control(mod_ctx, panel, &autoTargetControl, nullptr);

    UiControlDesc followControl = UI_CONTROL_DESC_INIT;
    followControl.kind = UI_CONTROL_TOGGLE;
    followControl.label = "Epona follows when dismounted";
    followControl.help_rml = "When on, Epona follows Link on foot once he dismounts, turning to "
                              "face him and walking over at a brisk walk (never a full gallop) "
                              "once he's more than about 1000 units away (half the clawshot's "
                              "maximum range), stopping at that distance. She keeps turning to "
                              "face him down to 750 units, then holds completely still. If he "
                              "gets more than about 5000 units away, she gives up following "
                              "until he comes back closer. Requires "
                              "\"Zelda active in combat\" to be on.";
    followControl.binding = UI_BINDING_CONFIG_VAR;
    followControl.config_var = g_cvarEponaFollowDismounted;
    followControl.is_disabled = is_epona_follow_control_disabled;
    svc_ui->pane_add_control(mod_ctx, panel, &followControl, nullptr);
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

    result = mods::hook::add_post<HorseSetStickData>(on_horse_set_stick_data_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's stick-data setter, she won't follow Link when he's "
            "dismounted: {}",
            (int)result);
    }

    result = mods::hook::add_post<HorseExecute>(on_horse_follow_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's execute() for the dismounted-follow facing correction: {}",
            (int)result);
    }

    result = mods::hook::add_post<HorseExecute>(on_horse_talk_face_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook Epona's execute() for the Zelda-talk facing correction, she won't "
            "turn to let Zelda look at Link if he talks to her from behind: {}",
            (int)result);
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

    result = mods::hook::add_pre<NpcZeldaDraw>(on_npc_zelda_draw_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook the Zelda NPC talk proxy's draw call, an invisible duplicate Zelda "
            "may become visible: {}",
            (int)result);
    }

    result = mods::hook::add_pre<NpcZeldaSetCollision>(on_npc_zelda_set_collision_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook the Zelda NPC talk proxy's collision setup, an invisible duplicate "
            "Zelda may physically push Link/Epona around: {}",
            (int)result);
    }

    result = mods::hook::add_post<NpcZeldaExecute>(on_npc_zelda_execute_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook the Zelda NPC talk proxy's execute(), talking to Zelda while "
            "dismounted will be unavailable: {}",
            (int)result);
    }

    result = mods::hook::add_pre<MsgFlowGetInitNodeIndex>(on_msg_flow_get_init_node_index_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook the message flow's starting-node lookup, talking to Zelda while "
            "dismounted will open an empty/incorrect conversation: {}",
            (int)result);
    }

    result = register_zelda_talk_flow();
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to register Zelda's dismounted-talk message, talking to Zelda while "
            "dismounted will be unavailable: {}",
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

    ConfigVarDesc followCvarDesc = CONFIG_VAR_DESC_INIT;
    followCvarDesc.name = "eponaFollowDismounted";
    followCvarDesc.type = CONFIG_VAR_BOOL;
    followCvarDesc.default_bool = false;
    result = svc_config->register_var(mod_ctx, &followCvarDesc, &g_cvarEponaFollowDismounted);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to register eponaFollowDismounted option, defaulting to following off: {}",
            (int)result);
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_mods_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    if (result != MOD_OK) {
        mods::log::warn("failed to register mods panel, the cutscene toggle won't be visible: {}",
                         (int)result);
    }

    ensure_light_arrow_particles_loaded();

    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daHorse_c* horse = dComIfGp_getHorseActor();

    if (!s_observedInitialHorseState) {
        s_observedInitialHorseState = true;
        s_safeToModifyEpona = (horse == nullptr);
    }

    if (!s_safeToModifyEpona) {
        if (horse != nullptr) {
            return MOD_OK;
        }
        s_safeToModifyEpona = true;
    }

    if (horse == nullptr) {
        remove_spawned_zelda();
        s_hideSpawnedZeldaInCutscene = false;
        daPy_py_c* link = daPy_getLinkPlayerActorClass();
        if (link != nullptr) {
            link->offHorseZelda();
        }
        update_zelda_talk_interaction();
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

    if (zelda_talk_event_active()) {
        s_hideSpawnedZeldaInCutscene = false;
    }

    log_cutscene_diagnostics_if_changed(horse, is_title_screen(), inScriptedCutscene,
                                         storyHoZeldaActive);

    update_zelda_talk_interaction();

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    remove_spawned_zelda();
    remove_zelda_npc_proxy();
    s_zeldaTalkGraph.reset();
    s_zeldaTalkMessages.clear();
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
    mods::hook::uninstall<NpcZeldaDraw>();
    mods::hook::uninstall<NpcZeldaSetCollision>();
    mods::hook::uninstall<NpcZeldaExecute>();
    mods::hook::uninstall<MsgFlowGetInitNodeIndex>();
    return MOD_OK;
}
}
