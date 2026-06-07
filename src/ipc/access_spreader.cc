/*
 * Slim port of folly::AccessSpreader. See access_spreader.h.
 */
#include "access_spreader.h"

#include "pfs_portability.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <sys/types.h>
#include <unistd.h>

namespace pfsutil {

namespace {

// Shim wrapping sched_getcpu in the getcpu(2) signature.
int sched_getcpu_shim(unsigned* cpu, unsigned* node, void* /*tcache*/) {
  int c = ::sched_getcpu();
  unsigned u = c >= 0 ? static_cast<unsigned>(c) : 0;
  if (cpu) {
    *cpu = u;
  }
  if (node) {
    *node = u;
  }
  return 0;
}

// Reads the first 64 bytes of `name` relative to `dirfd`. Returns empty on
// ENOENT; throws on any other error.
std::string read_short_file(int dirfd, const std::string& name) {
  int fd = ::openat(dirfd, name.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return {};
    }
    throw std::runtime_error(
        std::string("openat(") + name + "): " + std::strerror(errno));
  }
  alignas(64) char buf[64];
  ssize_t n;
  do {
    n = ::pread(fd, buf, sizeof(buf), 0);
  } while (n < 0 && errno == EINTR);
  ::close(fd);
  if (n < 0) {
    return {};
  }
  return std::string(buf, static_cast<size_t>(n));
}

// Returns the first decimal number in `line`, or throws if it doesn't lead
// with a number terminated by ',', '-', '\n', or end-of-string.
size_t parse_leading_number(const std::string& line) {
  const char* raw = line.c_str();
  char* end = nullptr;
  unsigned long val = std::strtoul(raw, &end, 10);
  if (end == raw ||
      (*end != ',' && *end != '-' && *end != '\n' && *end != '\0')) {
    throw std::runtime_error("error parsing list '" + line + "'");
  }
  return static_cast<size_t>(val);
}

bool proc_cpuinfo_line_relevant(const std::string& line) {
  return line.size() > 4 && (line[0] == 'p' || line[0] == 'c');
}

// Parse /proc/cpuinfo lines into (physicalId, coreId, cpu) tuples.
std::vector<std::tuple<size_t, size_t, size_t>> parse_proc_cpuinfo_lines(
    const std::vector<std::string>& lines) {
  std::vector<std::tuple<size_t, size_t, size_t>> cpus;
  size_t physicalId = 0;
  size_t coreId = 0;
  size_t maxCpu = 0;
  size_t numPhysicalIds = 0;
  size_t numCoreIds = 0;
  // Read in reverse so that "processor" emits a complete record.
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    const auto& line = *it;
    if (!proc_cpuinfo_line_relevant(line)) {
      continue;
    }
    auto sep = line.find(':');
    if (sep == std::string::npos || sep + 2 > line.size()) {
      continue;
    }
    auto arg = line.substr(sep + 2);
    if (line.find("physical id") == 0) {
      physicalId = parse_leading_number(arg);
      ++numPhysicalIds;
    } else if (line.find("core id") == 0) {
      coreId = parse_leading_number(arg);
      ++numCoreIds;
    } else if (line.find("processor") == 0) {
      auto cpu = parse_leading_number(arg);
      maxCpu = std::max(cpu, maxCpu);
      cpus.emplace_back(physicalId, coreId, cpu);
    }
  }
  if (cpus.empty()) {
    throw std::runtime_error("no CPUs parsed from /proc/cpuinfo");
  }
  if (maxCpu != cpus.size() - 1) {
    throw std::runtime_error(
        "offline CPUs not supported for /proc/cpuinfo cache locality source");
  }
  if (numPhysicalIds == 0) {
    throw std::runtime_error("no physical ids found");
  }
  if (numCoreIds == 0) {
    throw std::runtime_error("no core ids found");
  }
  return cpus;
}

} // namespace

// =============== CacheLocality ===============

CacheLocality::CacheLocality(std::vector<std::vector<size_t>> equivClasses) {
  numCpus = equivClasses.size();
  for (size_t cpu = 0; cpu < numCpus; ++cpu) {
    for (size_t level = 0; level < equivClasses[cpu].size(); ++level) {
      if (equivClasses[cpu][level] == cpu) {
        // Count each equivalence class once: when processing its representative.
        while (numCachesByLevel.size() <= level) {
          numCachesByLevel.push_back(0);
        }
        numCachesByLevel[level]++;
      }
    }
  }

  std::vector<size_t> cpus(numCpus);
  std::iota(cpus.begin(), cpus.end(), size_t{0});
  std::sort(cpus.begin(), cpus.end(), [&](size_t lhs, size_t rhs) {
    auto& l = equivClasses[lhs];
    auto& r = equivClasses[rhs];
    if (l.size() != r.size()) {
      return l.size() < r.size();
    }
    // Order by equiv class with highest index first (LLC), so cpus that share
    // the same LLC end up adjacent.
    for (size_t i = l.size(); i > 0; --i) {
      auto idx = i - 1;
      if (l[idx] != r[idx]) {
        return l[idx] < r[idx];
      }
    }
    return lhs < rhs;
  });

  // Inverse permutation: for each cpu, its position in the locality order.
  localityIndexByCpu.resize(numCpus);
  for (size_t i = 0; i < cpus.size(); ++i) {
    localityIndexByCpu[cpus[i]] = i;
  }
  equivClassesByCpu = std::move(equivClasses);
}

CacheLocality CacheLocality::uniform(size_t numCpus) {
  std::vector<std::vector<size_t>> equivClassesByCpu(numCpus, {0});
  return CacheLocality{std::move(equivClassesByCpu)};
}

CacheLocality CacheLocality::readFromProcCpuinfo() {
  std::vector<std::string> lines;
  std::ifstream xi("/proc/cpuinfo");
  if (xi.fail()) {
    throw std::runtime_error("unable to open /proc/cpuinfo");
  }
  char buf[8192];
  while (xi.good() && lines.size() < 20000) {
    xi.getline(buf, sizeof(buf));
    std::string str(buf);
    if (proc_cpuinfo_line_relevant(str)) {
      lines.emplace_back(std::move(str));
    }
  }
  return readFromProcCpuinfoLines(lines);
}

CacheLocality CacheLocality::readFromProcCpuinfoLines(
    const std::vector<std::string>& lines) {
  auto cpus = parse_proc_cpuinfo_lines(lines);
  std::sort(cpus.begin(), cpus.end());

  // We can't tell the real cache hierarchy from /proc/cpuinfo, so assume 3
  // levels: L1 and L2 per-core, L3 per-socket. The representative for each
  // L1 and L3 equivalence class is the first cpu in the class.
  std::vector<std::vector<size_t>> equivClassesByCpu(cpus.size());
  size_t l1Equiv = 0;
  size_t l3Equiv = 0;
  for (size_t i = 0; i < cpus.size(); ++i) {
    auto [physicalId, coreId, cpu] = cpus[i];
    if (i == 0 || physicalId != std::get<0>(cpus[i - 1]) ||
        coreId != std::get<1>(cpus[i - 1])) {
      l1Equiv = cpu;
    }
    if (i == 0 || physicalId != std::get<0>(cpus[i - 1])) {
      l3Equiv = cpu;
    }
    equivClassesByCpu[cpu] = {l1Equiv, l1Equiv, l3Equiv};
  }
  return CacheLocality{std::move(equivClassesByCpu)};
}

CacheLocality CacheLocality::readFromSysfsTree(const char* root) {
  std::vector<std::vector<size_t>> equivClassesByCpu;

  std::string base = root ? root : "";
  base += "/sys/devices/system/cpu";
  int allfd = ::open(base.c_str(), O_DIRECTORY | O_CLOEXEC);
  if (allfd < 0) {
    throw std::runtime_error(
        std::string("unable to open sysfs: ") + std::strerror(errno));
  }
  size_t maxindex = 0;
  for (size_t cpu = 0;; ++cpu) {
    std::string cpuroot = "cpu" + std::to_string(cpu) + "/cache";
    int cpufd = ::openat(allfd, cpuroot.c_str(), O_DIRECTORY | O_CLOEXEC);
    if (cpufd < 0) {
      if (errno == ENOENT) {
        break;
      }
      ::close(allfd);
      throw std::runtime_error(
          "openat(" + cpuroot + "): " + std::strerror(errno));
    }
    std::vector<size_t> levels;
    levels.reserve(maxindex);
    for (size_t index = 0;; ++index) {
      std::string dir = "index" + std::to_string(index) + "/";
      auto cacheType = read_short_file(cpufd, dir + "type");
      if (cacheType.empty()) {
        break;
      }
      if (cacheType[0] == 'I') {
        // {"Data","Instruction","Unified"}: skip the icache.
        continue;
      }
      auto equivStr = read_short_file(cpufd, dir + "shared_cpu_list");
      if (equivStr.empty()) {
        break;
      }
      levels.push_back(parse_leading_number(equivStr));
    }
    ::close(cpufd);
    maxindex = std::max(maxindex, levels.size());
    if (levels.empty()) {
      break;
    }
    equivClassesByCpu.emplace_back(std::move(levels));
  }
  ::close(allfd);
  if (equivClassesByCpu.empty()) {
    throw std::runtime_error("unable to load cache sharing info");
  }
  return CacheLocality{std::move(equivClassesByCpu)};
}

CacheLocality CacheLocality::readFromSysfs() {
  return readFromSysfsTree("");
}

const CacheLocality& CacheLocality::system() {
  static std::atomic<const CacheLocality*> cache{nullptr};
  auto value = cache.load(std::memory_order_acquire);
  if (value != nullptr) {
    return *value;
  }
  CacheLocality* next = nullptr;
  // Best info on Linux x86: try /proc/cpuinfo first (fast, hyperthread-aware),
  // then sysfs (slower but accurate), then a uniform fallback.
  try {
    next = new CacheLocality(readFromProcCpuinfo());
  } catch (...) {
    try {
      next = new CacheLocality(readFromSysfs());
    } catch (...) {
      long n = ::sysconf(_SC_NPROCESSORS_CONF);
      next = new CacheLocality(uniform(n > 0 ? static_cast<size_t>(n) : 32));
    }
  }
  if (cache.compare_exchange_strong(value, next, std::memory_order_acq_rel)) {
    return *next;
  }
  delete next;
  return *value;
}

// =============== Getcpu ===============

Getcpu::Func Getcpu::resolveVdsoFunc() {
  void* h = ::dlopen("linux-vdso.so.1", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  if (h == nullptr) {
    return nullptr;
  }
  auto func = reinterpret_cast<Getcpu::Func>(::dlsym(h, "__vdso_getcpu"));
  if (func == nullptr) {
    ::dlclose(h);
  }
  return func;
}

// =============== AccessSpreader ===============

int AccessSpreader::degenerateGetcpu(unsigned* cpu, unsigned* node, void*) {
  if (cpu) {
    *cpu = 0;
  }
  if (node) {
    *node = 0;
  }
  return 0;
}

Getcpu::Func AccessSpreader::pickGetcpuFunc() {
  auto best = Getcpu::resolveVdsoFunc();
  return best ? best : &sched_getcpu_shim;
}

void AccessSpreader::initialize(GlobalState& s) {
  const auto& cl = CacheLocality::system();
  const auto n = cl.numCpus;
  for (size_t width = 0; width <= kMaxCpus; ++width) {
    auto& row = s.table[width];
    auto numStripes = std::max(size_t{1}, width);
    for (size_t cpu = 0; cpu < kMaxCpus && cpu < n; ++cpu) {
      auto index = cl.localityIndexByCpu[cpu];
      assert(index < n);
      row[cpu] = static_cast<CompactStripe>((index * numStripes) / n);
      assert(row[cpu] < numStripes);
    }
    // Tile the pattern across the rest of the row by doubling.
    size_t filled = n;
    while (filled < kMaxCpus) {
      size_t len = std::min(filled, kMaxCpus - filled);
      for (size_t i = 0; i < len; ++i) {
        row[filled + i] = row[i];
      }
      filled += len;
    }
  }
  s.getcpu.exchange(pickGetcpuFunc(), std::memory_order_acq_rel);
}

AccessSpreader::GlobalState& AccessSpreader::state() {
  static GlobalState s{};
  if (unlikely(!s.getcpu.load(std::memory_order_acquire))) {
    initialize(s);
  }
  return s;
}

size_t AccessSpreader::current(size_t numStripes) {
  assert(numStripes > 0);
  const auto& s = state();
  unsigned cpu = 0;
  s.getcpu.load(std::memory_order_relaxed)(&cpu, nullptr, nullptr);
  cpu = cpu % kMaxCpus;
  return s.table[std::min(size_t(kMaxCpus), numStripes)][cpu];
}

unsigned AccessSpreader::CpuCache::cpu(GlobalState const& s) {
  if (unlikely(cachedCpuUses_-- == 0)) {
    unsigned c = 0;
    s.getcpu.load(std::memory_order_relaxed)(&c, nullptr, nullptr);
    cachedCpu_ = c % kMaxCpus;
    cachedCpuUses_ = kMaxCachedCpuUses - 1;
  }
  return cachedCpu_;
}

AccessSpreader::CpuCache& AccessSpreader::cpuCache() {
  static thread_local CpuCache c;
  return c;
}

size_t AccessSpreader::cachedCurrent(size_t numStripes) {
  assert(numStripes > 0);
  const auto& s = state();
  unsigned cpu = cpuCache().cpu(s);
  return s.table[std::min(size_t(kMaxCpus), numStripes)][cpu];
}

void AccessSpreader::invalidateCachedCurrent() {
  cpuCache().invalidate();
}

size_t AccessSpreader::localityIndexForStripe(
    size_t numStripes, size_t stripe) {
  assert(stripe < numStripes);
  return stripe *
      std::min(size_t(kMaxCpus), CacheLocality::system().numCpus) /
      numStripes;
}

namespace {

// Force AccessSpreader::initialize to run during static-init, before any
// thread races on state(). Mirrors folly's AccessSpreaderStaticInit.
struct StaticInit {
  StaticInit() { (void)AccessSpreader::current(~size_t(0)); }
};
StaticInit static_init;

} // namespace

} // namespace pfsutil
