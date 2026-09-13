savedcmd_ave_cmd.o := gcc -Wp,-MMD,./.ave_cmd.o.d -nostdinc -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/generated -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/uapi -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/generated/uapi -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi -I/usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/generated/uapi -include /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler-version.h -include /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/kconfig.h -include /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler_types.h -D__KERNEL__ -mlittle-endian -DCC_USING_PATCHABLE_FUNCTION_ENTRY -DKASAN_SHADOW_SCALE_SHIFT= -Wundef -DKBUILD_EXTRA_WARN1 -fshort-wchar -funsigned-char -fno-common -fno-PIE -fno-strict-aliasing -std=gnu11 -fms-extensions -mgeneral-regs-only -DCONFIG_CC_HAS_K_CONSTRAINT=1 -Wno-psabi -mabi=lp64 -fno-asynchronous-unwind-tables -fno-unwind-tables -mbranch-protection=pac-ret -Wa,-march=armv8.5-a -DARM64_ASM_ARCH='"armv8.5-a"' -DKASAN_SHADOW_SCALE_SHIFT= -fno-delete-null-pointer-checks -O2 -fno-allow-store-data-races -fstack-protector-strong -fno-omit-frame-pointer -fno-optimize-sibling-calls -ftrivial-auto-var-init=zero -fzero-init-padding-bits=all -fno-stack-clash-protection -fdiagnostics-show-context=2 -fpatchable-function-entry=4,2 -fno-inline-functions-called-once -fmin-function-alignment=8 -fstrict-flex-arrays=3 -fno-strict-overflow -fno-stack-check -fconserve-stack -fno-builtin-wcslen -Wall -Wextra -Wundef -Werror=implicit-function-declaration -Werror=implicit-int -Werror=return-type -Werror=strict-prototypes -Wno-format-security -Wno-trigraphs -Wno-frame-address -Wno-address-of-packed-member -Wmissing-declarations -Wmissing-prototypes -Wframe-larger-than=2048 -Wno-main -Wno-type-limits -Wno-dangling-pointer -Wvla-larger-than=1 -Wno-pointer-sign -Wcast-function-type -Wno-unterminated-string-initialization -Wno-array-bounds -Wno-stringop-overflow -Wno-alloc-size-larger-than -Wimplicit-fallthrough=5 -Werror=date-time -Werror=incompatible-pointer-types -Werror=designated-init -Wenum-conversion -Wunused -Wmissing-format-attribute -Wmissing-include-dirs -Wunused-const-variable -Wno-missing-field-initializers -Wno-shift-negative-value -Wno-maybe-uninitialized -Wno-sign-compare -Wno-unused-parameter -mstack-protector-guard=sysreg -mstack-protector-guard-reg=sp_el0 -mstack-protector-guard-offset=2168  -fsanitize=bounds-strict -fsanitize=shift    -DMODULE  -DKBUILD_BASENAME='"ave_cmd"' -DKBUILD_MODNAME='"apple_ave"' -D__KBUILD_MODNAME=apple_ave -c -o ave_cmd.o ave_cmd.c  

source_ave_cmd.o := ave_cmd.c

deps_ave_cmd.o := \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler-version.h \
    $(wildcard include/config/CC_VERSION_TEXT) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/kconfig.h \
    $(wildcard include/config/CPU_BIG_ENDIAN) \
    $(wildcard include/config/BOOGER) \
    $(wildcard include/config/FOO) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler_types.h \
    $(wildcard include/config/DEBUG_INFO_BTF) \
    $(wildcard include/config/PAHOLE_HAS_BTF_TAG) \
    $(wildcard include/config/FUNCTION_ALIGNMENT) \
    $(wildcard include/config/CC_HAS_SANE_FUNCTION_ALIGNMENT) \
    $(wildcard include/config/X86_64) \
    $(wildcard include/config/ARM64) \
    $(wildcard include/config/LD_DEAD_CODE_DATA_ELIMINATION) \
    $(wildcard include/config/LTO_CLANG) \
    $(wildcard include/config/HAVE_ARCH_COMPILER_H) \
    $(wildcard include/config/KCSAN) \
    $(wildcard include/config/CC_HAS_ASSUME) \
    $(wildcard include/config/CC_HAS_COUNTED_BY) \
    $(wildcard include/config/FORTIFY_SOURCE) \
    $(wildcard include/config/UBSAN_BOUNDS) \
    $(wildcard include/config/CC_HAS_COUNTED_BY_PTR) \
    $(wildcard include/config/CC_HAS_MULTIDIMENSIONAL_NONSTRING) \
    $(wildcard include/config/CFI) \
    $(wildcard include/config/ARCH_USES_CFI_GENERIC_LLVM_PASS) \
    $(wildcard include/config/CC_HAS_BROKEN_COUNTED_BY_REF) \
    $(wildcard include/config/CC_HAS_ASM_INLINE) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler-context-analysis.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler_attributes.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler-gcc.h \
    $(wildcard include/config/ARCH_USE_BUILTIN_BSWAP) \
    $(wildcard include/config/SHADOW_CALL_STACK) \
    $(wildcard include/config/KCOV) \
    $(wildcard include/config/CC_HAS_TYPEOF_UNQUAL) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/compiler.h \
    $(wildcard include/config/ARM64_PTR_AUTH_KERNEL) \
    $(wildcard include/config/ARM64_PTR_AUTH) \
    $(wildcard include/config/BUILTIN_RETURN_ADDRESS_STRIPS_PAC) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/errno.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/errno.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/generated/uapi/asm/errno.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/errno.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/errno-base.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/string.h \
    $(wildcard include/config/BINARY_PRINTF) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/args.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/array_size.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/compiler.h \
    $(wildcard include/config/TRACE_BRANCH_PROFILING) \
    $(wildcard include/config/PROFILE_ALL_BRANCHES) \
    $(wildcard include/config/OBJTOOL) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/rwonce.h \
    $(wildcard include/config/LTO) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/rwonce.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/kasan-checks.h \
    $(wildcard include/config/KASAN_GENERIC) \
    $(wildcard include/config/KASAN_SW_TAGS) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/types.h \
    $(wildcard include/config/HAVE_UID16) \
    $(wildcard include/config/UID16) \
    $(wildcard include/config/ARCH_DMA_ADDR_T_64BIT) \
    $(wildcard include/config/PHYS_ADDR_T_64BIT) \
    $(wildcard include/config/64BIT) \
    $(wildcard include/config/ARCH_32BIT_USTAT_F_TINODE) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/generated/uapi/asm/types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/int-ll64.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/int-ll64.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/uapi/asm/bitsperlong.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/bitsperlong.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/bitsperlong.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/posix_types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/stddef.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/stddef.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/uapi/asm/posix_types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/posix_types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/kcsan-checks.h \
    $(wildcard include/config/KCSAN_WEAK_MEMORY) \
    $(wildcard include/config/KCSAN_IGNORE_ATOMICS) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/cleanup.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/err.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/overflow.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/limits.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/limits.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/vdso/limits.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/const.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/vdso/const.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/const.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/stdarg.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/string.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/string.h \
    $(wildcard include/config/ARCH_HAS_UACCESS_FLUSHCACHE) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/fortify-string.h \
    $(wildcard include/config/CC_HAS_KASAN_MEMINTRINSIC_PREFIX) \
    $(wildcard include/config/GENERIC_ENTRY) \
    $(wildcard include/config/KMSAN) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/bug.h \
    $(wildcard include/config/GENERIC_BUG) \
    $(wildcard include/config/PRINTK) \
    $(wildcard include/config/BUG_ON_DATA_CORRUPTION) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/bug.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/stringify.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/asm-bug.h \
    $(wildcard include/config/DEBUG_BUGVERBOSE) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/brk-imm.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/bug.h \
    $(wildcard include/config/DEBUG_BUGVERBOSE_DETAILED) \
    $(wildcard include/config/BUG) \
    $(wildcard include/config/GENERIC_BUG_RELATIVE_POINTERS) \
    $(wildcard include/config/SMP) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/instrumentation.h \
    $(wildcard include/config/NOINSTR_VALIDATION) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/once_lite.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/panic.h \
    $(wildcard include/config/PANIC_TIMEOUT) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/printk.h \
    $(wildcard include/config/MESSAGE_LOGLEVEL_DEFAULT) \
    $(wildcard include/config/CONSOLE_LOGLEVEL_DEFAULT) \
    $(wildcard include/config/CONSOLE_LOGLEVEL_QUIET) \
    $(wildcard include/config/EARLY_PRINTK) \
    $(wildcard include/config/PRINTK_INDEX) \
    $(wildcard include/config/DYNAMIC_DEBUG) \
    $(wildcard include/config/DYNAMIC_DEBUG_CORE) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/init.h \
    $(wildcard include/config/MEMORY_HOTPLUG) \
    $(wildcard include/config/HAVE_ARCH_PREL32_RELOCATIONS) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/build_bug.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/kern_levels.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/linkage.h \
    $(wildcard include/config/ARCH_USE_SYM_ANNOTATIONS) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/export.h \
    $(wildcard include/config/MODVERSIONS) \
    $(wildcard include/config/GENDWARFKSYMS) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/linkage.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/ratelimit_types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/bits.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/vdso/bits.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/bits.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/param.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/uapi/asm/param.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/param.h \
    $(wildcard include/config/HZ) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/param.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/spinlock_types_raw.h \
    $(wildcard include/config/DEBUG_SPINLOCK) \
    $(wildcard include/config/DEBUG_LOCK_ALLOC) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/spinlock_types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/qspinlock_types.h \
    $(wildcard include/config/NR_CPUS) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/asm-generic/qrwlock_types.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/uapi/asm/byteorder.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/byteorder/little_endian.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/byteorder/little_endian.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/swab.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/linux/swab.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/generated/uapi/asm/swab.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/uapi/asm-generic/swab.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/byteorder/generic.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/lockdep_types.h \
    $(wildcard include/config/PROVE_RAW_LOCK_NESTING) \
    $(wildcard include/config/LOCKDEP) \
    $(wildcard include/config/LOCK_STAT) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/dynamic_debug.h \
    $(wildcard include/config/JUMP_LABEL) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/jump_label.h \
    $(wildcard include/config/HAVE_ARCH_JUMP_LABEL_RELATIVE) \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/jump_label.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/insn.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/arch/arm64/include/asm/insn-def.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/unaligned.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/linux/unaligned/packed_struct.h \
  /usr/src/kernels/7.1.13-401.asahi.vrr3.fc44.aarch64+16k/include/vdso/unaligned.h \
  ave_cmd.h \
  ave_abi.h \
    $(wildcard include/config/DONE) \
  ave_version.h \

ave_cmd.o: $(deps_ave_cmd.o)

$(deps_ave_cmd.o):
