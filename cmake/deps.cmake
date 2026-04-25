# Verify external libraries that are referenced by name from
# target_link_libraries() in the various subdirectories.  Without these
# explicit checks a missing dependency only surfaces at link time as a
# cryptic "cannot find -l<name>" error.  Failing early with a descriptive
# hint about which distribution package to install saves a lot of time
# for new contributors and packagers.

include(FindPackageHandleStandardArgs)
find_package(PkgConfig)

set(_PFSD_MISSING_DEPS "")

# ---------------------------------------------------------------------------
# glog (google-glog) --- used by pfs_core, pfsd, pfs_tools, pfs_fuse and folly.
# ---------------------------------------------------------------------------
# Prefer the upstream CMake package (glog >= 0.4 ships glog-config.cmake);
# fall back to a manual search so that older distro packages still work.
find_package(glog CONFIG QUIET)
if(NOT glog_FOUND)
    find_path(GLOG_INCLUDE_DIR glog/logging.h)
    find_library(GLOG_LIBRARY NAMES glog)
    find_package_handle_standard_args(glog
        REQUIRED_VARS GLOG_LIBRARY GLOG_INCLUDE_DIR)
    mark_as_advanced(GLOG_INCLUDE_DIR GLOG_LIBRARY)
endif()
if(NOT glog_FOUND AND NOT GLOG_FOUND)
    list(APPEND _PFSD_MISSING_DEPS
        "glog (google-glog): install libgoogle-glog-dev (Debian/Ubuntu) or glog-devel (RHEL/Rocky/Fedora)")
endif()

# ---------------------------------------------------------------------------
# libfmt --- used transitively through folly and linked into folly's targets.
# ---------------------------------------------------------------------------
find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
    find_path(FMT_INCLUDE_DIR fmt/core.h)
    find_library(FMT_LIBRARY NAMES fmt fmtd)
    find_package_handle_standard_args(fmt
        REQUIRED_VARS FMT_LIBRARY FMT_INCLUDE_DIR)
    mark_as_advanced(FMT_INCLUDE_DIR FMT_LIBRARY)
endif()
if(NOT fmt_FOUND AND NOT FMT_FOUND)
    list(APPEND _PFSD_MISSING_DEPS
        "libfmt: install libfmt-dev (Debian/Ubuntu) or fmt-devel (RHEL/Rocky/Fedora)")
endif()

# ---------------------------------------------------------------------------
# libfuse --- used by pfs_fuse (src/pfs_fuse/pfs_fuse.cc uses FUSE API v2.9).
# ---------------------------------------------------------------------------
set(FUSE_FOUND FALSE)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FUSE QUIET fuse)
endif()
if(NOT FUSE_FOUND)
    find_path(FUSE_INCLUDE_DIR fuse.h PATH_SUFFIXES fuse)
    find_library(FUSE_LIBRARY NAMES fuse)
    find_package_handle_standard_args(FUSE
        REQUIRED_VARS FUSE_LIBRARY FUSE_INCLUDE_DIR)
    mark_as_advanced(FUSE_INCLUDE_DIR FUSE_LIBRARY)
endif()
if(NOT FUSE_FOUND)
    list(APPEND _PFSD_MISSING_DEPS
        "libfuse (FUSE 2.x): install libfuse-dev (Debian/Ubuntu) or fuse-devel (RHEL/Rocky/Fedora)")
endif()

# ---------------------------------------------------------------------------
# Report all missing dependencies at once so that the user does not have to
# re-run cmake repeatedly to discover them one by one.
# ---------------------------------------------------------------------------
if(_PFSD_MISSING_DEPS)
    set(_msg "The following required dependencies were not found:")
    foreach(_dep IN LISTS _PFSD_MISSING_DEPS)
        set(_msg "${_msg}\n  * ${_dep}")
    endforeach()
    message(FATAL_ERROR "${_msg}")
endif()
