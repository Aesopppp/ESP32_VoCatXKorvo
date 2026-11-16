#pragma once
/* 本测试是确定性的单线程调度注入；此替身不验证多核原子性。固件使用 SDK 的
 * stdatomic.h，真实锁、调度和网络时序需实板验证。 */
#include <stdbool.h>
typedef bool atomic_bool;
typedef unsigned atomic_uint;
typedef bool atomic_flag;
#define ATOMIC_FLAG_INIT false
static inline bool atomic_flag_test_and_set(atomic_flag *p)
{ bool previous = *p; *p = true; return previous; }
static inline void atomic_flag_clear(atomic_flag *p) { *p = false; }
#define atomic_load(p) (*(p))
#define atomic_store(p, v) ((*(p)) = (v))
#define atomic_fetch_add(p, v) ((*(p) += (v)) - (v))
static inline unsigned atomic_exchange(atomic_uint *p, unsigned value)
{ unsigned previous = *p; *p = value; return previous; }
