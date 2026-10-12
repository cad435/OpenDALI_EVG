/*
    dali_cmd_scenes.c - Scene and group command handlers (IEC 62386-102)
*/

#include "dali_cmd_scenes.h"
#include "dali_fade.h"
#include "../dali_state.h"
#include "../dali_physical.h"
#include "../nvm/dali_nvm.h"
#include "../../logger.h"

void dali_scene_recall(uint8_t scene) {
    uint8_t slevel = ds.scene_level[scene];
    if (slevel == 0xFF) return;  /* MASK = not in scene */
    dali_fade_stop();
    ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
    slevel = clamp_level(slevel);
    uint32_t eff_fade_ms = dali_fade_get_effective_ms();
    if (eff_fade_ms == 0 || ds.actual_level == slevel) {
        ds.actual_level = slevel;
        if (ds.arc_callback) ds.arc_callback(ds.actual_level);
    } else {
        dali_fade_start(slevel, eff_fade_ms);
    }
}

void dali_scene_store(uint8_t scene) {
    ds.scene_level[scene] = ds.dtr0;
    nvm_mark_dirty();
    ds.reset_state = 0;
    LOG_CMD("SCENE%d=%d", scene, ds.dtr0);
}

void dali_scene_remove(uint8_t scene) {
    ds.scene_level[scene] = 0xFF;
    nvm_mark_dirty();
    ds.reset_state = 0;
    LOG_CMD("RMSCENE%d", scene);
}

void dali_group_add(uint8_t group) {
    ds.group_membership |= (1 << group);
    nvm_mark_dirty();
    ds.reset_state = 0;
    LOG_CMD("ADDGRP%d", group);
}

void dali_group_remove(uint8_t group) {
    ds.group_membership &= ~(1 << group);
    nvm_mark_dirty();
    ds.reset_state = 0;
    LOG_CMD("RMGRP%d", group);
}
