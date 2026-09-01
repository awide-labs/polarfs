# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic
Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- `fscp` no longer aborts with a spurious CRC error when copying a file
  whose last block holds only a few bytes of data — the data was
  intact, but the CRC helper could not handle blocks that short. The
  check now also really covers the first bytes of every block, which
  were skipped before (XCOM-183)

### Performance

- `fscp` copied far slower than it could (observed capped at ~400MB/s
  with 32 workers) because its buffers were not aligned for direct I/O:
  every block bounced through an extra copy, and every bounce's
  malloc/free became an mmap/munmap cycle due to the low mmap threshold,
  all serialized on a single process-wide lock. Buffers are now aligned
  and handed to the device directly (XCOM-184)

## [3.0.0] - 2026-07-14

### Changed

- `mountstat_enable` now defaults to off to remove per-I/O stats overhead;
  re-enable per mount when latency stats are needed (XCOM-176)
- `diskdev_trim_enable` now defaults to on so freed space is TRIMmed on the
  local block-device backend. The option is now a standard `0/1` switch, so
  `=0` disables and `=1` enables (previously `=0` was rejected and `=2`
  disabled it) (XCOM-176)

### Removed

- Removed the never-read `orphan_interval` and `orphan_select_max_num`
  options; these knobs had no effect (XCOM-176)
- Removed unregistered dead keys from `etc/polarfs.conf`:
  `paxos_wait_time`, `paxos_acquire_time`, `paxos_hold_time`,
  `io_wait_deadline`, `pangu_client_nthread`, `pangu_iodepth`, and
  `polar_iodepth` (XCOM-176)
- Dropped the dead `pfsd` command-line flags `-l` and `-i` (XCOM-176)

## [2.2.0] - 2026-07-13

### Added

- Zero-copy I/O path: callers register an fd-backed shared buffer
  (`memfd_create`/`shm_open`) via `pfsd_register_shared_buffer()` and run device
  I/O directly out of it through `pfsd_pread_zc`/`pfsd_pwrite_zc`, eliminating the
  memcpy into the PFS shared-memory pool that the ordinary read/write path
  incurs. Buffers can be registered/deregistered before or after mount (XCOM-175)

### Fixed

- Fixed mountstat file-type classification, which hardcoded the data
  directory component as `data` and so classified every file as `others`.
  Also extended the coverage to the remaining subdirectories that previously
  fell through to `others`: `pg_tblspc` (`tablespace`), `pg_twophase`
  (`twophase`), `pg_commit_ts` (`commit_ts`), `pg_multixact` (`multixact`),
  `pg_csnlog` (`csnlog`), `pg_replslot` (`replslot`), and `pg_bulkload`
  (`bulkload`); these also work as `pfsadm mountstat -f/-s` filter values
  (XCOM-27)

- Fix memory leak in pfsd_mount()/pfsd_umount() pair. (TTDB-1801)
- Fix memory leak and unreleased mutex on the `pfsd_sdk_init()` error
  paths after `pfs_mount_prepare()`. These fire when no daemon is
  running, e.g. the pfsd-less `pfs` tool mount that falls back to a
  local mount. (XCOM-179)
- Fix signed overflow in local_file_lseek() (mark as intended) (TTDB-1801)
- Make the `pfsadm` utility fully compatible with Python 3 (XCOM-177)

## [2.1.10] - 2026-06-07

### Fixed

- After a client crashed or was killed mid-handshake, pfsdaemon could keep
  that host id reserved even though no client was connected. Remounting the
  same host id then failed with "Repeat rw/ro mount with same hostid" until
  the daemon was restarted (XCOM-158)

## [2.1.9] - 2026-05-14

### Removed

- Build-time dependencies on folly, fmt, glog, double-conversion, and
  fast_float (XCOM-154)

## [2.1.8] - 2026-05-08

### Changed

- Release builds now also pass `-ffat-lto-objects` (GCC only) so that each `.o`
  carries both GIMPLE bytecode and a regular optimized `.text`. Consumers that
  link `libpfsd` without `-flto` then take the regular code path and skip the
  multi-second LTO codegen pass that the linker plugin would otherwise trigger
  for every consumer binary. Static archive size roughly doubles; release-build
  LTO behavior is unchanged. (XCOM-152)

## [2.1.7] - 2026-05-06

### Fixed

- After truncating a file down, a subsequent partial overwrite near the
  start of the last block could silently zero part of the file's surviving
  data; regression from XCOM-120 in 2.1.2 (XCOM-150)

## [2.1.6] - 2026-05-01

### Added

- `docker/builder/Dockerfile.ubuntu:devel` for builds on Ubuntu devel/rolling
  toolchains (XCOM-142)

### Changed

- Release builds use link-time optimization (LTO) by default via compile and
  link `-flto` flags (not CMake IPO, so vendored Folly stays unpatched); pass
  `-DPFSD_ENABLE_LTO=OFF` to CMake to disable (XCOM-148)
- Compile PolarFS C++ sources as C++20 to match vendored Folly requirements
  (XCOM-142)

### Fixed

- `StackAllocator` passes `uint8_t*` into `StackAllocatorState` so C++20
  `std::make_shared` construction checks succeed (XCOM-142)
- CMake minimum raised to 3.16 and `message()` quoting fixed for current CMake
  releases (XCOM-142)
- Vendored Folly pinned to upstream tag v2026.04.27.00 for current Boost
  and CMake behavior on Boost 1.89+ toolchains (XCOM-142)
- `autobuild.sh` now exits non-zero when CMake configure fails (XCOM-142)
- Renamed root `VERSION` to `POLARFS_VERSION` so project-root include paths
  do not shadow the C++20 `<version>` standard header (XCOM-142)

## [2.1.5] - 2026-04-25

### Changed

- CMake verifies glog, libfmt, and libfuse at configure time and reports
  missing dependencies with distro install hints instead of failing at link
  time with opaque errors (XCOM-139)

### Fixed

- pfsdaemon could crash when a client disconnected while it still had
  in-flight requests on the daemon (XCOM-135)
- pfsdaemon could misbehave or crash when a client disconnected before
  completing the initial handshake (XCOM-136)

## [2.1.4] - 2026-04-07

### Fixed

- pfsdaemon on replica could crash after a failed RW promote (e.g. triggered
  by `pfs mkdir`) left the internal mount entry in an inconsistent state
  (XCOM-132)

## [2.1.3] - 2026-04-03

### Fixed

- Inode LRU eviction in `pfs_put_inode` checked the caller's refcnt instead of
  the eviction candidate's, preventing cache eviction under concurrency and
  causing unbounded inode tree growth (XCOM-129)
- Reclaim glibc arena memory after unmount to prevent unbounded RSS growth
  across repeated mount/unmount cycles (XCOM-130)

## [2.1.2] - 2026-03-30

### Added

- Cross-host RW lease for shared-disk mutual exclusion, using a ballot-based
  protocol inspired by sanlock delta leases. New `pfs lease` diagnostic command
  (XCOM-90)

### Performance
- Eager block hole zero-fill on first write eliminates per-write metadata
  lock contention when filling preallocated blocks, improving concurrent
  write throughput up to ~16x (XCOM-120)
- Replace per-call zero buffer allocation in block I/O with a static
  preallocated buffer, avoiding malloc/memset/free on the write path (XCOM-2)

## [2.1.1] - 2026-03-24

### Fixed

- PolarFS used the `-march=native` compiler flags for released builds which was
causing SIGILL when running on older CPUs or in virtualized environments. Now
`-march=x86-64-v3 -mtune=haswell` is used instead (XCOM-122)

## [2.1.0] - 2026-02-09

### Added
- Add `diskdev_flush_enable` option to the block device backend, allowing
  FLUSH commands to be skipped on devices with Power Loss Protection (XCOM-50)

### Fixed
- Implement `pfsd_fsync()` and `pfs_fsync()` support for block device backend (XCOM-50)
- Fix IO buffer allocation bug in libpfsd leading to replication breaks in PolarDB (XCOM-56)
- Fix shared memory not being cleaned up during `pfsd_umount()` (XCOM-86)
- Fix signed overflow causing PFSD to crash with assertion failure when file is appended (XCOM-97)

## [2.0.0] - 2025-08-11

This is the first release by Awide Labs. Changes compared to the open source PolarFS
released by Alibaba (commit d0c5dc6):

### Added
- Initial version of RPM packaging (XCOM-46)
- PFSD SDK APIs for zero-copy read/write and shared memory buffer registration

### Fixed
- Port pfsadm to Python 3
- Align log buffer to prevent SIGSEGV (XCOM-7)

### Performance
- Parallel log header metadata check for RO mounts (XCOM-36)
- New IPC mechanism using MPMC queues and scalable rw-lock implementation
  (XCOM-9, XCOM-10, XCOM-11, XCOM-15)
- Use hardware-accelerated checksum calculation (XCOM-6)
- Cache getpid() to avoid syscalls on hot path (XCOM-14)
- Optimize server-side memory allocation statistics
- Optimize client connection management
- Optimize server inode cache locking
- Optimize global file descriptor table locking (XCOM-13)

### Removed
- Remove pfsdaemon options `-s` (worker sleep interval), `-b` (cpuset binding),
  and `-a` (shm directory), made obsolete by the new IPC implementation

[unreleased]: https://github.com/awide-labs/polarfs/compare/v3.0.0..master
[3.0.0]: https://github.com/awide-labs/polarfs/compare/v2.2.0..v3.0.0
[2.2.0]: https://github.com/awide-labs/polarfs/compare/v2.1.10..v2.2.0
[2.1.10]: https://github.com/awide-labs/polarfs/compare/v2.1.9..v2.1.10
[2.1.9]: https://github.com/awide-labs/polarfs/compare/v2.1.8..v2.1.9
[2.1.8]: https://github.com/awide-labs/polarfs/compare/v2.1.7..v2.1.8
[2.1.7]: https://github.com/awide-labs/polarfs/compare/v2.1.6..v2.1.7
[2.1.6]: https://github.com/awide-labs/polarfs/compare/v2.1.5..v2.1.6
[2.1.5]: https://github.com/awide-labs/polarfs/compare/v2.1.4..v2.1.5
[2.1.4]: https://github.com/awide-labs/polarfs/compare/v2.1.3..v2.1.4
[2.1.3]: https://github.com/awide-labs/polarfs/compare/v2.1.2..v2.1.3
[2.1.2]: https://github.com/awide-labs/polarfs/compare/v2.1.1..v2.1.2
[2.1.1]: https://github.com/awide-labs/polarfs/compare/v2.1.0..v2.1.1
[2.1.0]: https://github.com/awide-labs/polarfs/compare/v2.0.0..v2.1.0
[2.0.0]: https://github.com/awide-labs/polarfs/compare/d0c5dc6..v2.0.0
