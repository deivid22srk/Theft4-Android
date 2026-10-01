#pragma once
// RexGlue's PPC types are authoritative. graine SDK 0.7.5 renamed the generated
// project header from `gta4_config.h` → `gta4_init.h` and the image/code-size
// defines from `PPC_*` → `REX_*`. Keep the old PPC_* aliases so Liberty's
// kernel/memory.cpp + main.cpp don't need wholesale renames.
#include "../../glue/rexglue-sdk-main/gta4-recomp/generated/gta4_init.h"
#ifndef PPC_IMAGE_BASE
  #define PPC_IMAGE_BASE REX_IMAGE_BASE
  #define PPC_IMAGE_SIZE REX_IMAGE_SIZE
  #define PPC_CODE_BASE  REX_CODE_BASE
  #define PPC_CODE_SIZE  REX_CODE_SIZE
#endif
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

// SDK v0.7.4: Thread-local PPC context accessed via rex::runtime::current_ppc_context()
#include <rex/system/thread_state.h>

// Legacy XenosRecomp-era PPC_* macro surface kept for Liberty's kernel/*.h,
// patches/*.cpp and cpu/*.h callers; the 0.7.5 generator emits the REX_*
// forms instead. All aliases below are guarded so a future ppc_config.h
// that reintroduces the PPC_* names wins.
#ifndef PPC_FUNC
#define PPC_FUNC(x) void x(PPCContext& __restrict ctx, uint8_t* base)
#define PPC_FUNC_IMPL(x) extern "C" PPC_FUNC(x)
#define PPC_WEAK_FUNC(x) __attribute__((weak, noinline)) PPC_FUNC(x)
#define PPC_EXTERN_FUNC(x) extern PPC_FUNC(x)
#endif

#ifndef PPC_FUNC_PROLOGUE
#define PPC_FUNC_PROLOGUE() REX_FUNC_PROLOGUE()
#endif

#ifndef PPC_LOAD_U8
#define PPC_LOAD_U8(x) REX_LOAD_U8(x)
#define PPC_LOAD_U16(x) REX_LOAD_U16(x)
#define PPC_LOAD_U32(x) REX_LOAD_U32(x)
#define PPC_LOAD_U64(x) REX_LOAD_U64(x)
#define PPC_LOAD_STRING(x, len) REX_LOAD_STRING(x, len)
#define PPC_STORE_U8(x, y) REX_STORE_U8(x, y)
#define PPC_STORE_U16(x, y) REX_STORE_U16(x, y)
#define PPC_STORE_U32(x, y) REX_STORE_U32(x, y)
#define PPC_STORE_U64(x, y) REX_STORE_U64(x, y)
#endif

#ifndef PPC_MEMORY_SIZE
#define PPC_MEMORY_SIZE REX_MEMORY_SIZE
#endif

#ifndef PPC_LOOKUP_FUNC
#define PPC_LOOKUP_FUNC(x, y) REX_LOOKUP_FUNC(x, y)
#define PPC_CALL_INDIRECT_FUNC(x) REX_CALL_INDIRECT_FUNC(x)
#endif

inline PPCContext* GetPPCContext()
{
    return rex::runtime::current_ppc_context();
}

inline void SetPPCContext(PPCContext& /* ctx */)
{
    // In 0.7.4, the context is per-thread via ThreadState::Bind().
    // SetPPCContext is a no-op — the context is managed by the runtime.
}
