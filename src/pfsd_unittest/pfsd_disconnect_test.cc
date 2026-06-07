/*
 * pfsd_disconnect_test — the daemon must release a client's mount reference
 * when the connection drops on the socket *error* path (onError), not only on
 * a clean EOF (onEof).
 *
 * onError used to only log and do nothing, so a client that had already taken
 * a reference (via CLIENT_HELLO -> pfs_mount_acquire) and then died abruptly
 * stranded its host slot with refcnt 1, and every later mount on it was
 * rejected ("Repeat rw mount with same hostid").  (The clean-EOF release is
 * already covered by pfsd_ebusy_test's promote-crash mode.)
 *
 * The check:
 *   - child : raw-connect, drain the daemon's REGISTER_QUEUES (so its
 *             accept-time write succeeds and it goes on to read our hello),
 *             send a RW CLIENT_HELLO on <hostid>, then close WITHOUT reading
 *             the reply.  The daemon reads the buffered hello, runs
 *             pfs_mount_acquire (takes the reference), and its SERVER_HELLO
 *             write then meets the gone peer -> EPIPE -> onError, with the
 *             reference held.  This ordering matters, both learned the hard
 *             way: aborting before draining races the daemon's accept-time
 *             write, which fails first and tears the connection down before
 *             the hello is read (no reference, nothing to leak); and onError
 *             only fires on a daemon *write* -- on this AF_UNIX even an
 *             abortive close surfaces to the daemon's read as EOF -> onEof,
 *             which has always released the reference and would mask the bug.
 *   - parent: mount <hostid> RW via the SDK.  If onError leaked the reference
 *             the daemon rejects this; if it released, the mount succeeds.
 *             (A crashed daemon also makes this fail/hang.)
 *
 * Standalone gtest binary, NOT part of pfsd_filetest: the SDK keeps a
 * process-wide singleton session and pfsd_sdk_init short-circuits once inited,
 * so the verify mount must run in a process that hasn't mounted yet.  We fork
 * before the parent touches the SDK so the child stays SDK-free.
 *
 * Requires a running pfsdaemon (started by test/run.sh).
 * Usage: pfsd_disconnect_test <cluster> <pbdname> <hostid>
 * (<hostid> must be within the device's slot count -- mkfs default is 30.)
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pfsd_sdk.h"
#include "pfs_mount.h"
#include "ipc/proto.h"

namespace
{

std::string g_cluster;
std::string g_pbdname;
int g_hostid = -1;

constexpr int kRecvTimeoutSec = 10;

/*
 * Read until one complete framed message has arrived, then return.  The
 * daemon's first message after accept is REGISTER_QUEUES; draining it serves
 * two purposes: it proves the daemon's accept-time write happened while we
 * were still connected (otherwise that write fails first and tears the
 * connection down before our hello is ever read -- no reference taken), and
 * it leaves us positioned to send the hello.  Using read() (not recvmsg())
 * makes the kernel discard the message's SCM_RIGHTS fds for us.  Returns false
 * on EOF / timeout.
 */
bool
drain_one_message(int fd)
{
	std::vector<uint8_t> buf;
	uint8_t chunk[4096];
	for (;;) {
		if (buf.size() >= sizeof(uint32_t) * 2) {
			int type = 0;
			size_t msgLen = 0;
			if (ipc::readMessageTypeAndLen(
				buf.data(), buf.size(), type, msgLen) &&
			    msgLen >= sizeof(uint32_t) * 2 &&
			    buf.size() >= msgLen)
				return true;
		}
		ssize_t n = ::read(fd, chunk, sizeof(chunk));
		if (n > 0) {
			buf.insert(buf.end(), chunk, chunk + n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return false;
	}
}

/*
 * Make the daemon take a RW reference on <hostid> and then meet the
 * socket-error path (onError) when it replies.
 *
 * onError fires only when the daemon WRITES to a gone peer (on this AF_UNIX an
 * abortive close still surfaces as EOF to the peer's read -> onEof, which
 * already releases the ref and would mask the bug).  So: drain the daemon's
 * REGISTER_QUEUES first (so its accept-time write succeeds and it goes on to
 * read our hello), send the hello, then close WITHOUT reading the reply.  The
 * daemon reads the buffered hello, runs pfs_mount_acquire (takes the ref), and
 * its SERVER_HELLO write then hits the closed peer -> EPIPE -> onError, with
 * the reference held.  Runs in the child; always _exit()s.
 *   _exit(0) drove the handshake; _exit(3) setup failed.
 */
[[noreturn]] void
hello_then_abort(
    const std::string &cluster, const std::string &pbdname, int hostid)
{
	int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		_exit(3);

	struct timeval tv;
	tv.tv_sec = kRecvTimeoutSec;
	tv.tv_usec = 0;
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_un addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	std::string sockPath = ipc::makeSockPath(pbdname);
	std::strncpy(
	    addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
	if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr),
		sizeof(addr)) != 0)
		_exit(3);

	/* Let the daemon's accept-time REGISTER_QUEUES write land before we
	 * abort. */
	if (!drain_one_message(fd))
		_exit(3);

	/* Reuse the production serializer so the wire format can't drift. */
	ipc::ClientHelloMessage hello;
	hello.cluster = cluster;
	hello.pbdname = pbdname;
	hello.host_id = hostid;
	hello.flags = MNTFLG_RD | MNTFLG_WR | MNTFLG_LOG;
	std::vector<uint8_t> bytes = hello.serialize();

	size_t off = 0;
	while (off < bytes.size()) {
		ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			_exit(3);
		}
		off += static_cast<size_t>(n);
	}

	/* Close without reading the reply: the daemon's SERVER_HELLO write
	 * (after it has taken the reference) meets the gone peer -> EPIPE ->
	 * onError. */
	::close(fd);
	_exit(0);
}

} // namespace

/*
 * Acquire-then-abort on the error path, then a clean RW mount on the same
 * host id must succeed: the reference must have been released.
 */
TEST(PfsdDisconnect, ErrorPathReleasesReference)
{
	/* Fork before any SDK use so the child never initialises the SDK. */
	pid_t pid = fork();
	ASSERT_GE(pid, 0) << "fork: " << std::strerror(errno);
	if (pid == 0)
		hello_then_abort(g_cluster, g_pbdname, g_hostid);

	int status = 0;
	ASSERT_EQ(pid, waitpid(pid, &status, 0)) << std::strerror(errno);
	ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
	    << "crash client could not take+abort a reference (status "
	    << status << ") -- check the host id is valid and the daemon is up";

	/*
	 * The daemon's event loop is single-threaded, so it finishes the
	 * child's connection (mount_acquire + onError release) before it
	 * processes our mount below — no settle delay needed.  If onError
	 * leaked the reference, pfs_host_incref rejects this mount.
	 */
	int r = pfsd_mount(g_cluster.c_str(), g_pbdname.c_str(), g_hostid,
	    MNTFLG_RD | MNTFLG_WR | MNTFLG_LOG);
	EXPECT_EQ(0, r) << "mount rejected (errno " << errno
			<< ") — reference leaked on socket-error disconnect";
	if (r == 0)
		pfsd_umount_force(g_pbdname.c_str());
}

int
main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <cluster> <pbdname> <hostid>\n",
		    argv[0]);
		return 2;
	}
	g_cluster = argv[1];
	g_pbdname = argv[2];
	g_hostid = atoi(argv[3]);

	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
