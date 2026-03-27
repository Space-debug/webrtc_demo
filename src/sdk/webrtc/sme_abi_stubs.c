#if defined(__aarch64__)
// libyuv rotate_sme.o 引用 __arm_tpidr2_save（Clang SME / compiler-rt）。
// 当前工具链无对应 builtins 时提供空实现；运行时仅在选用 SME 路径时才会用到，
// 无 SME 的 CPU 上 libyuv 不会进入该路径。
__attribute__((visibility("default"))) void __arm_tpidr2_save(void) {}
#endif
