/*
 * Stub implementations for PPC functions that are declared via PPC_FUNC_IMPL
 * and referenced by hooks, but NOT present in the codegen output
 * (gta4_recomp.*.cpp). Without the original PPC binary decompilation for
 * these addresses, we provide no-op stubs so the link succeeds. Hooks that
 * call through to these originals will effectively skip the original behavior.
 *
 * BUILD-1.0 UPDATE (recomp regenerated from the user's ISO default.xex):
 * Every address below that the build-1.0 codegen now produces was REMOVED
 * from this file — keeping it would be a duplicate strong definition of the
 * generated __imp__ symbol and would break the link (or silently shadow the
 * real function). The v8-era stub list is preserved in git history.
 *
 * Currently empty: after the symbol migration, every __imp__ referenced by
 * the kept runtime files resolves to a generated function.
 */

#include "ppc_context.h"
