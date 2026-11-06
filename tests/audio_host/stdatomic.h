#pragma once
/* 本测试是确定性的单线程调度注入；此替身不验证多核原子性。固件使用 SDK 的
 * stdatomic.h，真实锁、调度和网络时序需实板验证。 */
#include <stdbool.h>
typedef bool atomic_bool;
typedef unsigned atomic_uint;
#define atomic_load(p) (*(p))
#define atomic_store(p, v) ((*(p)) = (v))
#define atomic_fetch_add(p, v) ((*(p) += (v)) - (v))
