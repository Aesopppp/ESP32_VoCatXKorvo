/* 使用生产 BOOT 映射和 YMOS 策略的编译期回归；不模拟 GPIO/RTOS。 */
#include "../main/boot_button_policy.h"

namespace {
constexpr bool HoldAndRelease(uint32_t base = 0)
{
    auto p = button_policy_initial();
    if (boot_button_policy_feed(&p, false, base) != BUTTON_ACTION_NONE) return false;
    boot_button_policy_feed(&p, false, base + 40);
    boot_button_policy_feed(&p, true, base + 100);
    boot_button_policy_feed(&p, true, base + 140);
    if (boot_button_policy_feed(&p, true, base + 3139) != BUTTON_ACTION_NONE) return false;
    if (boot_button_policy_feed(&p, true, base + 3140) != BUTTON_ACTION_PROVISION) return false;
    if (boot_button_policy_feed(&p, true, base + 9000) != BUTTON_ACTION_NONE) return false;
    boot_button_policy_feed(&p, false, base + 9100);
    if (boot_button_policy_feed(&p, false, base + 9140) != BUTTON_ACTION_NONE) return false;
    boot_button_policy_feed(&p, true, base + 9200);
    boot_button_policy_feed(&p, true, base + 9240);
    return boot_button_policy_feed(&p, true, base + 12240) == BUTTON_ACTION_PROVISION;
}

constexpr bool StartupHeld()
{
    auto p = button_policy_initial();
    boot_button_policy_feed(&p, true, 0);
    boot_button_policy_feed(&p, true, 40);
    if (boot_button_policy_feed(&p, true, 8000) != BUTTON_ACTION_NONE) return false;
    boot_button_policy_feed(&p, false, 8100);
    boot_button_policy_feed(&p, false, 8140);
    boot_button_policy_feed(&p, true, 8200);
    boot_button_policy_feed(&p, true, 8240);
    return boot_button_policy_feed(&p, true, 11240) == BUTTON_ACTION_PROVISION;
}

constexpr bool ShortPressAndBounce()
{
    auto p = button_policy_initial();
    boot_button_policy_feed(&p, false, 0);
    boot_button_policy_feed(&p, false, 40);
    boot_button_policy_feed(&p, true, 100);
    boot_button_policy_feed(&p, false, 120);
    boot_button_policy_feed(&p, true, 130);
    boot_button_policy_feed(&p, true, 170);
    // 两秒按压不能触发 REC 的结束对话；松开也不能触发短按对话。
    if (boot_button_policy_feed(&p, true, 2170) != BUTTON_ACTION_NONE) return false;
    boot_button_policy_feed(&p, false, 2500);
    if (boot_button_policy_feed(&p, false, 2540) != BUTTON_ACTION_NONE) return false;
    boot_button_policy_feed(&p, true, 2700);
    boot_button_policy_feed(&p, true, 2740);
    if (boot_button_policy_feed(&p, true, 5739) != BUTTON_ACTION_NONE) return false;
    return boot_button_policy_feed(&p, true, 5740) == BUTTON_ACTION_PROVISION;
}
}

static_assert(HoldAndRelease(), "BOOT: 3s boundary, one event per hold, rearm after release");
static_assert(HoldAndRelease(UINT32_MAX - 1000u), "BOOT: millisecond counter wraparound");
static_assert(StartupHeld(), "BOOT: held at task start must be released first");
static_assert(ShortPressAndBounce(), "BOOT: debounce, no short-press action or accumulated hold");
