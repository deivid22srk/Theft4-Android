/**
 * @file        core/memory_android.cpp
 * @brief       Android (bionic / NDK) memory backend
 *
 * Mirrors the POSIX backend for virtual allocation (mmap anonymous private),
 * but replaces the shm_open-based FileMappingHandle path with Android-native
 * shared memory primitives:
 *   - API 26+: ASharedMemory_create (libandroid.so, dlsym'd at runtime)
 *   - API <26: /dev/ashmem with ASHMEM_SET_NAME / ASHMEM_SET_SIZE
 *
 * shm_open is not usable on Android — /dev/shm does not exist on any device
 * shipping today, and bionic's shm_open/shm_unlink stubs return ENOSYS on
 * most API levels. ASharedMemory is the sanctioned equivalent.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// memfd_create flags (linux/memfd.h). Declare inline — bionic only exposes
// them from API 28 and we build against older sysroots too.
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef __NR_memfd_create
// arm64 syscall table: memfd_create = 279 (Linux 3.17+).
#define __NR_memfd_create 279
#endif

// /dev/ashmem ioctls. Not all NDK sysroots expose <linux/ashmem.h>, so declare
// the bits we need inline — they have been ABI-stable since Android 2.x.
#ifndef ASHMEM_NAME_LEN
#define ASHMEM_NAME_LEN 256
#endif
#ifndef __ASHMEMIOC
#define __ASHMEMIOC 0x77
#endif
#ifndef ASHMEM_SET_NAME
#define ASHMEM_SET_NAME _IOW(__ASHMEMIOC, 1, char[ASHMEM_NAME_LEN])
#endif
#ifndef ASHMEM_SET_SIZE
#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC, 3, size_t)
#endif

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/string.h>

namespace rex {
namespace memory {

namespace {

// Synchronous logcat breadcrumbs that survive even when the caller is about
// to std::_Exit() (the async spdlog sinks can lose the last lines). Same
// pattern as the main.cpp boot instrumentation.
#define MEM_ALOGE(...) \
  __android_log_print(ANDROID_LOG_ERROR, "rex.mem", __VA_ARGS__)
#define MEM_ALOGI(...) \
  __android_log_print(ANDROID_LOG_INFO, "rex.mem", __VA_ARGS__)

// Cache the device API level. android_get_device_api_level() is available from
// NDK r11+ / API 24+; for older API levels fall back to parsing the build
// property (not strictly needed here since the rest of LibertyRecomp requires
// much higher API levels).
int AndroidApiLevel() {
  static int level = []() {
#if __ANDROID_API__ >= 24
    return android_get_device_api_level();
#else
    return __ANDROID_API__;
#endif
  }();
  return level;
}

// libandroid.so dlopen handle and resolved ASharedMemory_create pointer.
// Loaded lazily on first CreateFileMappingHandle call.
using ASharedMemoryCreateFn = int (*)(const char*, size_t);
ASharedMemoryCreateFn LoadASharedMemoryCreate() {
  static ASharedMemoryCreateFn fn = []() -> ASharedMemoryCreateFn {
    if (AndroidApiLevel() < 26) return nullptr;
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return nullptr;
    return reinterpret_cast<ASharedMemoryCreateFn>(dlsym(lib, "ASharedMemory_create"));
  }();
  return fn;
}

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3) return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open()) return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e)) continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
}

bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0) return false;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) return false;
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open()) return false;
  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e)) continue;
    if (e.end <= cursor) continue;
    if (e.start > cursor) return false;
    cursor = e.end;
    if (cursor >= end) return true;
  }
  return cursor >= end;
}

PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';
  if (!r && !w && !x) return PageAccess::kNoAccess;
  if (x) return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

}  // namespace

size_t page_size() { return static_cast<size_t>(getpagesize()); }
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:        return PROT_NONE;
    case PageAccess::kReadOnly:        return PROT_READ;
    case PageAccess::kReadWrite:       return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly: return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
  // Android SELinux policy blocks PROT_WRITE|PROT_EXEC on most API levels for
  // apps without the execmem permission. Report false so upstream code takes
  // the W^X path (separate RW and RX mappings of a single fd).
  return false;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
  if (base_address) flags |= MAP_FIXED_NOREPLACE;
#else
  if (base_address) flags |= MAP_FIXED;
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    return result;
  }

#if defined(MAP_FIXED_NOREPLACE)
  // Commit-on-reservation path: a prior Reserve already holds this VA as
  // PROT_NONE; upgrade it with mprotect.
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    if (IsRangeFullyMapped(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        return base_address;
      }
    }
  }
#endif
  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      if (mprotect(base_address, length, PROT_NONE) != 0) return false;
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease:
      return munmap(base_address, length) == 0;
    default:
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
    LinuxMapEntry e;
    if (FindEntryForAddress(base_address, e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
  return mprotect(base_address, length, ToPosixProtectFlags(access)) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  length = 0;

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) return false;

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);
  return true;
}

// ── File mapping (shared memory) ─────────────────────────────────────────────

// WHY THE FALLBACK CHAIN BELOW EXISTS (device-verified, Android 14 / SDK 34):
//
// The guest address-space reservation needs a ~4.5 GiB fd-backed shared
// mapping (Xenia-style aliased views). libandroid's ASharedMemory_create()
// routes into libcutils ashmem_create_region(), which only uses memfd when
// the VENDOR property sys.use_memfd is true — the default is FALSE. The
// non-memfd path does open("/dev/ashmem"), and untrusted_app SELinux policy
// has had NO open permission on ashmem_device since Android 10:
//   avc: denied { open } for path="/dev/ashmem" scontext=u:r:untrusted_app
//        tcontext=u:object_r:ashmem_device:s0 tclass=chr_file permissive=0
// So on those devices ASharedMemory_create ALWAYS fails for apps and the
// guest memory never reserves (boot aborts with
// "Unable to reserve the 4gb guest address space").
//
// Order (first success wins; every failure logs a breadcrumb with errno):
//   1. memfd_create via direct syscall  — no SELinux restriction for apps,
//      kernel 3.17+, supports multi-GiB sparse ftruncate and aliased
//      MAP_SHARED views. This is the path that works everywhere.
//   2. ASharedMemory_create             — works when the vendor enables
//      sys.use_memfd (or future Android where libcutils flips to memfd).
//   3. /dev/ashmem ioctls               — pre-Android-10 devices only.
//   4. sparse file in the app cache dir — guaranteed fallback; needs
//      REX_ANDROID_CACHE_DIR (set by jni_glue). Pages are F2FS/ext4, so this
//      is slower than memfd but fully functional.
FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)access;
  (void)commit;

  const char* name = path.c_str();

  // Tier 1: memfd_create (direct syscall; the bionic wrapper needs API 28).
  errno = 0;
  int fd = static_cast<int>(syscall(__NR_memfd_create, name,
                                    MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (fd >= 0) {
    if (ftruncate(fd, static_cast<off_t>(length)) == 0) {
      MEM_ALOGI("guest memory: memfd_create('%s', %zu bytes) OK", name, length);
      return static_cast<FileMappingHandle>(fd);
    }
    MEM_ALOGE("guest memory: memfd ftruncate(%zu) failed: %s", length, strerror(errno));
    close(fd);
  } else {
    MEM_ALOGE("guest memory: memfd_create failed: %s — trying ASharedMemory", strerror(errno));
  }

  // Tier 2: ASharedMemory_create (API 26+, libandroid.so).
  if (auto create = LoadASharedMemoryCreate(); create != nullptr) {
    errno = 0;
    int sfd = create(name, length);
    if (sfd >= 0) {
      MEM_ALOGI("guest memory: ASharedMemory_create('%s', %zu bytes) OK", name, length);
      return static_cast<FileMappingHandle>(sfd);
    }
    MEM_ALOGE("guest memory: ASharedMemory_create failed: %s — trying /dev/ashmem",
              strerror(errno));
  }

  // Tier 3: legacy /dev/ashmem (pre-Android-10 SELinux; blocked on modern).
  errno = 0;
  int afd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
  if (afd >= 0) {
    char name_buf[ASHMEM_NAME_LEN];
    std::snprintf(name_buf, sizeof(name_buf), "%s", name);
    name_buf[ASHMEM_NAME_LEN - 1] = '\0';
    if (ioctl(afd, ASHMEM_SET_NAME, name_buf) == 0 &&
        ioctl(afd, ASHMEM_SET_SIZE, length) == 0) {
      MEM_ALOGI("guest memory: /dev/ashmem('%s', %zu bytes) OK", name, length);
      return static_cast<FileMappingHandle>(afd);
    }
    MEM_ALOGE("guest memory: /dev/ashmem ioctl failed: %s", strerror(errno));
    close(afd);
  } else {
    MEM_ALOGE("guest memory: open(/dev/ashmem) failed: %s — trying cache-dir file",
              strerror(errno));
  }

  // Tier 4: sparse file in the app-private cache directory. The fd is unlinked
  // immediately — the mapping lives as long as the fd does.
  const char* cache_dir = std::getenv("REX_ANDROID_CACHE_DIR");
  if (cache_dir && *cache_dir) {
    std::string file_path = std::string(cache_dir) + "/" + name + ".bin";
    errno = 0;
    int ffd = open(file_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (ffd >= 0) {
      unlink(file_path.c_str());
      if (ftruncate(ffd, static_cast<off_t>(length)) == 0) {
        MEM_ALOGI("guest memory: cache file mapping '%s' (%zu bytes) OK", file_path.c_str(),
                  length);
        return static_cast<FileMappingHandle>(ffd);
      }
      MEM_ALOGE("guest memory: cache file ftruncate(%zu) failed: %s", length,
                strerror(errno));
      close(ffd);
    } else {
      MEM_ALOGE("guest memory: open(%s) failed: %s", file_path.c_str(), strerror(errno));
    }
  }

  MEM_ALOGE("guest memory: ALL backends failed to reserve %zu bytes for '%s'", length, name);
  return kFileMappingHandleInvalid;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)path;
  int fd = static_cast<int>(handle);
  if (fd >= 0) {
    close(fd);
  }
  // No shm_unlink equivalent — ashmem/ASharedMemory fds self-delete on last
  // close across all mappings.
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  const size_t page = page_size();
  if (file_offset % page != 0) return nullptr;

  int flags = MAP_SHARED;
  if (base_address) {
    // Fixed-address views replace the anonymous reservation placed during heap
    // setup; MAP_FIXED is required.
    flags |= MAP_FIXED;
  }

  void* result = mmap(base_address, length, ToPosixProtectFlags(access), flags,
                      static_cast<int>(handle), static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) return nullptr;

  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
