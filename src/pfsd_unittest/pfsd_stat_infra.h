/*
 * pfsd_stat_infra - test infrastructure for devstat and mountstat.
 * Provides a way to run a set of file operations and to collect and
 * parse the devstat / mountstat counters from the pfsadm output.
 *
 * pfsadm iostat returns raw absolute counters only for the op count.
 * Latency and byte totals are not exact. Window latency cannot be
 * recovered cleanly, and bytes only with some error, so a check of
 * the returned byte count needs a GAP_BYTES margin.
 *
 * pfsadm mountstat returns per-second counters for a window, delayed
 * by about 2 s: "pfsadm mountstat -c 1 -b <t0> -r <range>". Each
 * second carries the op count plus the average latency and average
 * IO size, and the window total is recovered from those. Some
 * operations have no IO size.
 */

#ifndef	__PFSD_STAT_INFRA_H__
#define	__PFSD_STAT_INFRA_H__

#include <cstdint>
#include <functional>
#include <map>
#include <string>

struct MountstatOp {
	int64_t		ops = 0;
	int64_t		total_bytes = 0;
	double		total_latency = 0;
};

struct MountstatData {
	std::map<std::string, std::map<std::string, MountstatOp>> per_type;
	std::map<std::string, MountstatOp> summarize;

	const MountstatOp *get(const char *ftype, const char *op) const;
	const MountstatOp *get_sum(const char *op) const;
};

struct DevstatDelta {
	std::string	device_name;
	int64_t		read_ops = 0;
	int64_t		write_ops = 0;
	double		read_bytes = 0;
	double		write_bytes = 0;
};

struct RunResult {
	DevstatDelta	devstat;
	MountstatData	mountstat;
};

bool set_pfsd_config(const std::string &pbdname, const char *name, int value);

/*
 * Run a workload inside a measured statistics window:
 *
 *	sleep(quiet) -> t0 -> [iostat base] -> ops() -> sleep(settle)
 *	-> [iostat end + delta] -> [mountstat -b t0 -r (now-t0+2)]
 */
bool run_io_window(const std::string &pbdname, const std::function<void()> &ops,
    bool want_devstat, bool want_mountstat, RunResult *result, std::string *err);

void expect_row(const MountstatOp *o, const char *what, int64_t ops);
void expect_row_min(const MountstatOp *o, const char *what, int64_t min_ops);
void expect_row_io(const MountstatOp *o, const char *what, int64_t ops, int64_t min_bytes);
void expect_no_row(const MountstatOp *o, const char *what);

#endif	/* __PFSD_STAT_INFRA_H__ */
