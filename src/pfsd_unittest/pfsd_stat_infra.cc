#include "pfsd_stat_infra.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <unistd.h>

static const char PFSADM_BIN[] = "./bin/pfsadm";

static const int MOUNTSTAT_RANGE_EXTRA_S = 2;
static const int QUIET_S = 3;
static const int SETTLE_S = 5;

struct DevstatSnapshot {
	std::string	device_name;
	int64_t		read_ops = 0;
	int64_t		write_ops = 0;
	double		read_mbytes = 0;
	double		write_mbytes = 0;
};

static int
run_command_capture(const std::string &cmd, std::string *out)
{
	FILE *fp = popen(cmd.c_str(), "r");
	if (fp == nullptr)
		return -1;

	char buf[4096];
	while (size_t n = fread(buf, 1, sizeof(buf), fp))
		out->append(buf, n);

	return pclose(fp);
}

static bool
take_iostat_snapshot(const std::string &pbdname, DevstatSnapshot *snap, std::string *err)
{
	std::string	cmd = std::string(PFSADM_BIN) + " iostat -c 1 " + pbdname + " 2>&1";
	std::string	out;

	*snap = DevstatSnapshot();

	int rc = run_command_capture(cmd, &out);
	if (rc != 0) {
		*err = "pfsadm iostat failed (exit status " + std::to_string(rc) + "):\n" + out;
		return false;
	}

	/* pfsadm may emit NUL bytes in the device name field. */
	for (char &c : out)
		if (c == '\0')
			c = ' ';

	if (out.empty()) {
		*err = "pfsadm iostat produced no output";
		return false;
	}

	for (size_t pos = 0; pos < out.size(); ) {
		size_t eol = out.find('\n', pos);
		std::string line = (eol == std::string::npos) ? out.substr(pos)
		    : out.substr(pos, eol - pos);
		pos = (eol == std::string::npos) ? out.size() : eol + 1;

		if (line.empty() || line.rfind("device", 0) == 0)
			continue;

		char	devname[256];
		long long r_ops = 0, w_ops = 0;
		double	mrs = 0, mws = 0;
		int	n = std::sscanf(line.c_str(), "%255s %lld %lld %lf %lf",
		    devname, &r_ops, &w_ops, &mrs, &mws);
		if (n == 5) {
			snap->device_name  = devname;
			snap->read_ops     = r_ops;
			snap->write_ops    = w_ops;
			snap->read_mbytes  = mrs;
			snap->write_mbytes = mws;
			return true;
		}
	}

	*err = "could not parse iostat output:\n" + out;
	return false;
}

static bool
collect_mountstat(const std::string &pbdname, int64_t begin_time, int range,
    MountstatData *data, std::string *err)
{
	std::string	cmd = std::string(PFSADM_BIN) + " mountstat " + pbdname +
	    " -c 1 -b " + std::to_string((long long)begin_time) + " -r " +
	    std::to_string(range) + " 2>&1";
	std::string	out;

	*data = MountstatData();

	int rc = run_command_capture(cmd, &out);
	if (rc != 0) {
		*err = "pfsadm mountstat failed (exit status " + std::to_string(rc) + "):\n" + out;
		return false;
	}

	/*
	 * Timestamp is "Mon dd hh:mm:ss". n == 5 is a File_type row
	 * (numeric io_size); n == 4 and ftype "all" is Summarize.
	 */
	for (size_t pos = 0; pos < out.size(); ) {
		size_t eol = out.find('\n', pos);
		std::string line = (eol == std::string::npos) ? out.substr(pos)
		    : out.substr(pos, eol - pos);
		pos = (eol == std::string::npos) ? out.size() : eol + 1;

		if (line.empty())
			continue;

		char		ftype[32];
		char		opname[32];
		long long	ops = 0;
		double		lat = 0, iosize = 0;
		int		n = std::sscanf(line.c_str(),
		    "%*s %*d %*s %31s %31s %lld %lf %lf",
		    ftype, opname, &ops, &lat, &iosize);
		if (n < 4)
			continue;

		MountstatOp *o;
		if (n == 5)
			o = &data->per_type[ftype][opname];
		else if (std::strcmp(ftype, "all") == 0)
			o = &data->summarize[opname];
		else
			continue;

		/* Per-second averages -> window totals. */
		o->ops += ops;
		o->total_bytes += (int64_t)(ops * iosize);
		o->total_latency += ops * lat;
	}

	return true;
}

bool
set_pfsd_config(const std::string &pbdname, const char *name, int value)
{
	std::string cmd = std::string(PFSADM_BIN) + " config -s " + pbdname + " " + name +
	    " " + std::to_string(value) + " 2>&1";

	return system(cmd.c_str()) == 0;
}

const MountstatOp *
MountstatData::get(const char *ftype, const char *op) const
{
	auto ft = per_type.find(ftype);

	if (ft == per_type.end())
		return nullptr;

	auto opit = ft->second.find(op);
	return opit == ft->second.end() ? nullptr : &opit->second;
}

const MountstatOp *
MountstatData::get_sum(const char *op) const
{
	auto it = summarize.find(op);
	return it == summarize.end() ? nullptr : &it->second;
}

bool
run_io_window(const std::string &pbdname, const std::function<void()> &ops,
    bool want_devstat, bool want_mountstat, RunResult *result, std::string *err)
{
	DevstatSnapshot	base, end;

	*result = RunResult();

	sleep(QUIET_S);
	int64_t t0 = (int64_t)time(nullptr);

	if (want_devstat && !take_iostat_snapshot(pbdname, &base, err))
		return false;

	ops();

	sleep(SETTLE_S);

	if (want_devstat) {
		DevstatDelta	*d = &result->devstat;

		if (!take_iostat_snapshot(pbdname, &end, err))
			return false;

		d->device_name = end.device_name;
		d->read_ops = end.read_ops - base.read_ops;
		d->write_ops = end.write_ops - base.write_ops;
		d->read_bytes = (end.read_mbytes - base.read_mbytes) * 1024 * 1024;
		d->write_bytes = (end.write_mbytes - base.write_mbytes) * 1024 * 1024;
	}

	if (want_mountstat) {
		int64_t	now = (int64_t)time(nullptr);
		int	range = (int)(now - t0) + MOUNTSTAT_RANGE_EXTRA_S;

		if (!collect_mountstat(pbdname, t0, range, &result->mountstat, err))
			return false;
	}

	return true;
}

void
expect_row(const MountstatOp *o, const char *what, int64_t ops)
{
	if (o == nullptr) {
		ADD_FAILURE() << "mountstat " << what << " row is missing";
		return;
	}

	EXPECT_EQ(o->ops, ops) << "mountstat " << what << " ops != " << ops
	    << " (got " << o->ops << ")";
}

void
expect_row_min(const MountstatOp *o, const char *what, int64_t min_ops)
{
	if (o == nullptr) {
		ADD_FAILURE() << "mountstat " << what << " row is missing";
		return;
	}
	EXPECT_GE(o->ops, min_ops) << "mountstat " << what << " ops too low (got "
	    << o->ops << ", want >= " << min_ops << ")";
}

void
expect_row_io(const MountstatOp *o, const char *what, int64_t ops, int64_t min_bytes)
{
	expect_row(o, what, ops);
	if (o == nullptr)
		return;
	EXPECT_GT(o->total_latency, 0) << "mountstat " << what
	    << " total latency is zero";
	EXPECT_GE(o->total_bytes, min_bytes) << "mountstat " << what
	    << " total_bytes too small (got " << o->total_bytes << ")";
}

void
expect_no_row(const MountstatOp *o, const char *what)
{
	if (o != nullptr)
		ADD_FAILURE() << "unexpected mountstat " << what << " row";
}

