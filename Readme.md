<div align="center">

# Awide PolarFS

**A high-performance user-space POSIX distributed file system for cloud-native databases**

[![official site](https://img.shields.io/badge/official%20site-blueviolet?style=flat)](https://awide.tech/awidepolar)

[![GitHub License](https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat)](./LICENSE)
[![github-issues](https://img.shields.io/github/issues/awide-labs/polarfs?style=flat&logo=github)](https://github.com/awide-labs/polarfs/issues)
[![github-pullrequest](https://img.shields.io/github/issues-pr/awide-labs/polarfs?style=flat&logo=github)](https://github.com/awide-labs/polarfs/pulls)
[![github-forks](https://img.shields.io/github/forks/awide-labs/polarfs?style=flat&logo=github)](https://github.com/awide-labs/polarfs/network/members)
[![github-stars](https://img.shields.io/github/stars/awide-labs/polarfs?style=flat&logo=github)](https://github.com/awide-labs/polarfs/stargazers)

</div>

## Overview

Awide PolarFS (PFS) is a high-performance, user-space distributed file system
that complies with the POSIX standard. It is maintained by [Awide
Labs](https://awide.tech) and provides shared-storage I/O for [Awide
Polar](https://github.com/awide-labs/polar). This project is derived from the
[PolarDB File System](https://github.com/ApsaraDB/PolarDB-FileSystem), an
open-source file system originally released by Alibaba Cloud.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Quick Start

Awide PolarFS uses the background process **_pfsdaemon_** to provide services.
It is developed and tested on Rocky Linux and Ubuntu. Theoretically, it can
also be built on other Linux distributions.

### Install Dependencies

In the following example, Rocky Linux 9 is selected. Before you build Awide
PolarFS, install the following software:

- [CMake](https://cmake.org/): version 2.8 or later
- [GCC or G++](http://www.gnu.org/software/gcc/): version 4.8.5 or later
- [zlog](https://github.com/HardySimpson/zlog/releases): version 1.2.12 or later
- [libaio-devel](https://pagure.io/libaio)

We recommend that you use `yum` or `apt-get` to install CMake, GCC or G++, and
libaio-devel.

To install zlog, download the source code and run `make && sudo make install`.
zlog is installed in `/usr/local/lib`. If dynamic libraries cannot be located
when pfsdaemon is running, run `ldconfig` to refresh the loader cache.

We also provide RPM and DEB packages. If you install from a package, skip the
**Compile** and **Install pfsdaemon** sections below.

### Compile

After the dependencies are installed, go to the root directory of the source
tree and run:

```
./autobuild.sh
```

### Install pfsdaemon

Installing or uninstalling pfsdaemon requires root privileges.

After you compile Awide PolarFS, run:

```
sudo ./install.sh
```

### Run pfsdaemon

##### 1. Format the storage devices

First, find the existing block devices:

```
lsblk
```

Then select the block device to format, such as `nvme1n1`, and run:

```
sudo pfs -C disk mkfs nvme1n1
```

##### 2. Start pfsdaemon

```
sudo /usr/local/polarstore/pfsd/bin/start_pfsd.sh -p nvme1n1
```

`-p nvme1n1` specifies the device name and is required.

Optional parameters:

```
-f (not daemon mode)
-w #nworkers
-c log_config_file
-b (if bind cpuset)
-e db ins id
-a shm directory
-i #inode_list_size
```

##### 3. Stop pfsdaemon

```
sudo /usr/local/polarstore/pfsd/bin/stop_pfsd.sh nvme1n1
```

##### 4. Clean up runtime files

After stopping pfsdaemon, clear temporary files, logs, and shared memory files:

```
sudo /usr/local/polarstore/pfsd/bin/clean_pfsd.sh nvme1n1
```

##### 5. Verify PFS is running

Perform common file operations to verify that PFS is running as expected. See
the [PFS tool manual](docs/PFS_Tools-EN.md).

Example:

```
sudo pfs -C disk touch /nvme1n1/hello.txt
sudo pfs -C disk ls /nvme1n1/
```

### Uninstall pfsdaemon

##### 1. Stop pfsdaemon

```
sudo /usr/local/polarstore/pfsd/bin/stop_pfsd.sh nvme1n1
```

##### 2. Uninstall

```
sudo ./uninstall.sh
```

## FUSE

See [Readme-FUSE.md](Readme-FUSE.md) for mounting Awide PolarFS through FUSE.

## Documentation

- [PFS tool manual](docs/PFS_Tools-EN.md)

## Contributing

We welcome contributions! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## Software License

Awide PolarFS is released under the
[GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html)
(AGPLv3). See [LICENSE](./LICENSE) for the full license text.

This project is developed from PolarFS by Alibaba Cloud, which is licensed
under the Apache License 2.0. Portions of the codebase retain that upstream
license as described in [NOTICE](./NOTICE). Awide PolarFS also contains
third-party components under other open source licenses; see [NOTICE](./NOTICE)
for details.

## Contact

For product information, see the [Awide Polar website](https://awide.tech/awidepolar).

For support or contribution questions, open an issue or email `info@awide.io`.
