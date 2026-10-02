/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/main_android.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <android/log.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <linux/ashmem.h>

// memfd_create flags (linux/memfd.h). Declared inline — bionic only exposes
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

// Synchronous logcat breadcrumbs that survive even when the caller is about
// to std::_Exit() (the async spdlog sinks can lose the last lines).
#define REX_MEM_ALOGE(...) \
  __android_log_print(ANDROID_LOG_ERROR, "rex.mem", __VA_ARGS__)
#define REX_MEM_ALOGI(...) \
  __android_log_print(ANDROID_LOG_INFO, "rex.mem", __VA_ARGS__)

// TODO(tomc): Android or maybe na. idk
// #include "xenia/base/main_android.h"
#endif

namespace rex {
namespace memory {

// Convert filesystem path to valid shm_open name (must start with /, no other slashes)
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/')
      c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
  return true;
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// Find the mapping entry in /proc/self/maps that contains the given address
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;

  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (e.end <= cursor)
      continue;
    if (e.start > cursor)
      return false;  // gap found
    cursor = e.end;
    if (cursor >= end)
      return true;
  }
  return cursor >= end;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
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

  // Build flags - always use MAP_FIXED_NOREPLACE for fixed addresses
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    return result;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
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
      // Decommit: remove access first, then release physical pages
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      return munmap(base_address, length) == 0;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    LinuxMapEntry e;
    if (FindEntryForAddress(base_address, e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(base_address, length, prot) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // WHY THE FALLBACK CHAIN BELOW EXISTS (device-verified, Android 14 / SDK 34):
  //
  // ASharedMemory_create() routes into libcutils ashmem_create_region(),
  // which only uses memfd when the VENDOR property sys.use_memfd is true
  // (default FALSE). The non-memfd path open()s /dev/ashmem, and untrusted_app
  // SELinux policy has had NO open permission on ashmem_device since Android
  // 10 (kernel AVC: denied { open } for path="/dev/ashmem"
  // tcontext=u:object_r:ashmem_device:s0 tclass=chr_file). On such devices
  // this function used to ALWAYS fail for apps and the 4.5 GiB guest address
  // space could never be reserved (boot aborted with "Unable to reserve the
  // 4gb guest address space").
  //
  // Order (first success wins; every failure logs a breadcrumb with errno):
  //   1. memfd_create via direct syscall  — no SELinux restriction for apps,
  //      kernel 3.17+, supports multi-GiB sparse ftruncate and aliased
  //      MAP_SHARED views. This is the path that works everywhere.
  //   2. ASharedMemory_create             — works when the vendor enables
  //      sys.use_memfd (or future Android where libcutils flips to memfd).
  //      NOTE: android_ASharedMemory_create_ is only resolved if something
  //      calls memory::AndroidInitialize(); it is null otherwise.
  //   3. /dev/ashmem ioctls               — pre-Android-10 devices only.
  //   4. sparse file in the app cache dir — guaranteed fallback; needs
  //      REX_ANDROID_CACHE_DIR (set by LibertyRecomp jni_glue). Pages are
  //      F2FS/ext4, so this is slower than memfd but fully functional.
  const std::string shm_name_str = path.string();
  const char* shm_name = shm_name_str.c_str();

  // Tier 1: memfd_create (direct syscall; the bionic wrapper needs API 28).
  errno = 0;
  int memfd = static_cast<int>(syscall(__NR_memfd_create, shm_name,
                                       MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (memfd >= 0) {
    if (ftruncate(memfd, static_cast<off_t>(length)) == 0) {
      REX_MEM_ALOGI("guest memory: memfd_create('%s', %zu bytes) OK", shm_name, length);
      return static_cast<FileMappingHandle>(memfd);
    }
    REX_MEM_ALOGE("guest memory: memfd ftruncate(%zu) failed: %s", length, strerror(errno));
    close(memfd);
  } else {
    REX_MEM_ALOGE("guest memory: memfd_create failed: %s — trying ASharedMemory",
                  strerror(errno));
  }

  // Tier 2: ASharedMemory_create (API 26+, libandroid.so).
  if (android_ASharedMemory_create_) {
    errno = 0;
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    if (sharedmem_fd >= 0) {
      REX_MEM_ALOGI("guest memory: ASharedMemory_create('%s', %zu bytes) OK", shm_name, length);
      return static_cast<FileMappingHandle>(sharedmem_fd);
    }
    REX_MEM_ALOGE("guest memory: ASharedMemory_create failed: %s — trying /dev/ashmem",
                  strerror(errno));
  }

  // Tier 3: /dev/ashmem (pre-Android-10 SELinux; blocked on modern devices).
  errno = 0;
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR | O_CLOEXEC);
  if (ashmem_fd >= 0) {
    char ashmem_name[ASHMEM_NAME_LEN];
    strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
    if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) == 0 &&
        ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) == 0) {
      REX_MEM_ALOGI("guest memory: /dev/ashmem('%s', %zu bytes) OK", shm_name, length);
      return static_cast<FileMappingHandle>(ashmem_fd);
    }
    REX_MEM_ALOGE("guest memory: /dev/ashmem ioctl failed: %s", strerror(errno));
    close(ashmem_fd);
  } else {
    REX_MEM_ALOGE("guest memory: open(/dev/ashmem) failed: %s — trying cache-dir file",
                  strerror(errno));
  }

  // Tier 4: sparse file in the app-private cache directory. The file is
  // unlinked immediately — the mapping lives as long as the fd does.
  const char* cache_dir = std::getenv("REX_ANDROID_CACHE_DIR");
  if (cache_dir && *cache_dir) {
    std::string file_path = std::string(cache_dir) + "/" + path.string() + ".bin";
    errno = 0;
    int file_fd = open(file_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file_fd >= 0) {
      unlink(file_path.c_str());
      if (ftruncate(file_fd, static_cast<off_t>(length)) == 0) {
        REX_MEM_ALOGI("guest memory: cache file mapping '%s' (%zu bytes) OK",
                      file_path.c_str(), length);
        return static_cast<FileMappingHandle>(file_fd);
      }
      REX_MEM_ALOGE("guest memory: cache file ftruncate(%zu) failed: %s", length,
                    strerror(errno));
      close(file_fd);
    } else {
      REX_MEM_ALOGE("guest memory: open(%s) failed: %s", file_path.c_str(),
                    strerror(errno));
    }
  }

  REX_MEM_ALOGE("guest memory: ALL backends failed to reserve %zu bytes for '%s'", length,
                shm_name);
  return kFileMappingHandleInvalid;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                        static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
