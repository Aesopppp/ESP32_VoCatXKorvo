#ifndef BOOT_BUTTON_POLICY_H
#define BOOT_BUTTON_POLICY_H

#include "button_policy.h"

/* 复用 YMOS 消抖/长按策略。BOOT 对应 SET 长按，不产生短按对话、
 * 音量或长按结束对话动作。上电按住需先松手，再开始新操作。
 */
#ifdef __cplusplus
#define BOOT_CONSTEXPR constexpr
#else
#define BOOT_CONSTEXPR
#endif

static inline BOOT_CONSTEXPR button_action_t boot_button_policy_feed(
    button_policy_t *policy, bool pressed, uint32_t now_ms)
{
    return button_policy_feed(policy, pressed ? BUTTON_SET : BUTTON_NONE, now_ms);
}

#undef BOOT_CONSTEXPR
#endif
