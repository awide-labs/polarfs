# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic
Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

This is the first release by AwydeX. Changes compared to the open source PolarFS
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

[2.1.2]: https://github.com/Awydex/polarfs/compare/896b394..ad120c4
[2.1.1]: https://github.com/Awydex/polarfs/compare/3a3b0c6..896b394
[2.1.0]: https://github.com/Awydex/polarfs/compare/d563a18..3a3b0c6
[2.0.0]: https://github.com/Awydex/polarfs/compare/d0c5dc6..d563a18
