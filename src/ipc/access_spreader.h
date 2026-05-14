/*
 * Port of folly::AccessSpreader. Returns a stripe in [0, numStripes)
 * derived from the current CPU. Threads on the same socket / cache cluster
 * map to neighboring stripes (sysfs- or /proc/cpuinfo-aware), reducing
 * cross-cache traffic on striped data structures.
 *
 * See third-party/folly/folly/concurrency/CacheLocality.h for the original.
 * Trimmed down: drops the Atom template parameter (production always uses
 * std::atomic), drops LLCAccessSpreader and CoreAllocator, drops mobile-
 * specific HashingThreadId fallback.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pfsutil {

// CacheLocality describes the per-cpu cache topology of the current host.
// equivClassesByCpu[cpu] is a list of cache equivalence-class identifiers,
// one per cache level (closest-to-cpu first). Two cpus that share a cache at
// some level have the same identifier at that level.
struct CacheLocality {
  size_t numCpus = 0;
  std::vector<size_t> numCachesByLevel;
  std::vector<size_t> localityIndexByCpu;
  std::vector<std::vector<size_t>> equivClassesByCpu;

  // Returns the best locality info available, cached. Falls back through
  // /proc/cpuinfo, sysfs, and finally a uniform layout.
  static const CacheLocality& system();

  // Builds a degenerate one-cache layout for `numCpus`.
  static CacheLocality uniform(size_t numCpus);

  // Reader entry points. Each throws on failure.
  static CacheLocality readFromSysfs();
  static CacheLocality readFromSysfsTree(const char* root);
  static CacheLocality readFromProcCpuinfo();
  static CacheLocality readFromProcCpuinfoLines(
      const std::vector<std::string>& lines);

private:
  explicit CacheLocality(std::vector<std::vector<size_t>> equivClasses);
};

// Function pointer with the same signature as getcpu(2).
struct Getcpu {
  using Func = int (*)(unsigned* cpu, unsigned* node, void* unused);

  // Loads __vdso_getcpu via dlopen/dlsym. Returns nullptr if unavailable.
  static Func resolveVdsoFunc();
};

class AccessSpreader {
public:
  // If there are more cpus than this, no crash, just unnecessary sharing.
  static constexpr size_t kMaxCpus = 256;

  // Returns the stripe associated with the current CPU. Always < numStripes.
  static size_t current(size_t numStripes);

  // Like current() but caches the cpu in TLS for a small number of calls;
  // result may be slightly stale but ~5x cheaper.
  static size_t cachedCurrent(size_t numStripes);

  // Forces the next cachedCurrent() in this thread to re-probe.
  static void invalidateCachedCurrent();

  // Canonical index in [0, kMaxCpus) for each stripe. Lets two stripings
  // share global data structures.
  static size_t localityIndexForStripe(size_t numStripes, size_t stripe);

  static constexpr size_t maxStripeValue() { return kMaxCpus; }
  static constexpr size_t maxLocalityIndexValue() { return kMaxCpus; }

private:
  using CompactStripe = uint8_t;
  static_assert(
      kMaxCpus - 1 <= UINT8_MAX,
      "stripeByCpu element type isn't wide enough");
  static_assert(
      (kMaxCpus & (kMaxCpus - 1)) == 0,
      "kMaxCpus should be a power of two so modulo is fast");

  using CompactStripeTable = CompactStripe[kMaxCpus + 1][kMaxCpus];

  struct GlobalState {
    mutable CompactStripeTable table;
    std::atomic<Getcpu::Func> getcpu; // nullptr -> not initialized
  };

  static GlobalState& state();
  static void initialize(GlobalState& state);

  static int degenerateGetcpu(unsigned* cpu, unsigned* node, void*);
  static Getcpu::Func pickGetcpuFunc();

  class CpuCache {
  public:
    unsigned cpu(GlobalState const& s);
    void invalidate() { cachedCpuUses_ = 0; }

  private:
    static constexpr unsigned kMaxCachedCpuUses = 32;
    unsigned cachedCpu_ = 0;
    unsigned cachedCpuUses_ = 0;
  };

  static CpuCache& cpuCache();
};

} // namespace pfsutil
