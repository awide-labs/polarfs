# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic
Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[unreleased]: https://github.com/Awydex/polarfs/compare/d563a18..master
[2.0.0]: https://github.com/Awydex/polarfs/compare/d0c5dc6..d563a18
