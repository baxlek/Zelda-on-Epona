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

static s32 s_lastKnownZeldaRoomNo = -1;
static bool s_zeldaLostInSameRoom = false;

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

    daHorse_c* horse = dComIfGp_getHorseActor();
    bool inCutscene = horse != nullptr && horse->checkHorseDemoMode() && !is_title_screen();

    if (auto_target_enemies_enabled() && !inCutscene) {
        zelda->mGndAcKeep.setData(find_nearest_enemy(zelda));
    } else {
        zelda->mGndAcKeep.clearData();
    }
    return HOOK_CONTINUE;
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

    if (horse->getZeldaActor() == static_cast<fopAc_ac_c*>(zelda)) {
        horse->setZeldaActor(nullptr);
    }
    horse->setReinPosNormal();
}

static constexpr u8 kArrowWaitFrames = 5;

static constexpr f32 kDrawAnimSpeedMultiplier = 2.0f;

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

    arrow->current.angle.x = -dir.atan2sY_XZ();
    arrow->current.angle.y = dir.atan2sX_Z();
    arrow->shape_angle.y = arrow->current.angle.y;

    dir.normalizeZP();
    arrow->speed = dir * arrow->field_0x99c;
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

static const u32 kLightArrowHeapSize = 0x200000;

static void* s_lightArrowHeapBacking = nullptr;
static JKRExpHeap* s_lightArrowHeap = nullptr;

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

    dPa_control_c::getEmitterManager()->entryResourceManager(mgr, kLightArrowResMgrId);
    s_lightArrowResMgr = mgr;
    s_lightArrowJpcReady = true;

    mods::log::info("Pscene181.jpc loaded as particle bank {}, resource heap has {} bytes free",
                     kLightArrowResMgrId, heap->getFreeSize());
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

    result = mods::hook::replace<HoZeldaSetAnm>(on_hozelda_set_anm_replace);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook HoZelda setAnm, auto-targeting enemies will be unavailable: {}",
            (int)result);
    }

    result = mods::hook::add_post<ArrowShooting>(on_arrow_shooting_post);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook arrow shooting, auto-targeted Light Arrows may fly straight instead "
            "of toward their target: {}",
            (int)result);
    }

    result = mods::hook::add_pre<CcAtCheck>(on_cc_at_check_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook attack-power check, Light Arrows won't one-hit-kill high-health "
            "enemies: {}",
            (int)result);
    }

    result = mods::hook::add_pre<ParticleEntryResourceManager>(on_particle_entry_resource_manager_pre);
    if (result != MOD_OK) {
        mods::log::warn(
            "failed to hook particle emitter manager resource registration, Light Arrow particles "
            "outside the horseback duel will be unavailable: {}",
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

    mods::log::info("zelda_on_epona initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daHorse_c* horse = dComIfGp_getHorseActor();

    if (horse == nullptr) {
        remove_spawned_zelda();
        s_hideSpawnedZeldaInCutscene = false;
        daPy_getLinkPlayerActorClass()->offHorseZelda();
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

    if (is_spawned_zelda_attached(horse)) {
        daPy_getLinkPlayerActorClass()->offHorseZelda();
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
    mods::hook::uninstall<HoZeldaExecute>();
    mods::hook::uninstall<HoZeldaSetAnm>();
    mods::hook::uninstall<ArrowShooting>();
    return MOD_OK;
}
}
