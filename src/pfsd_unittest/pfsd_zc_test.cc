/*
 * pfsd_zc_test — exercises the explicit zero-copy ("zc") SDK data path:
 * pfsd_alloc / pfsd_free hand out a slice of a shared-memory pool, and
 * pfsd_pwrite_zc / pfsd_pread_zc let the daemon read/write that slice in
 * place (no per-IO copy across the socket). The buffer id + pool-relative
 * offset travel in the request; the daemon resolves them against its own
 * mapping of the same memfd.
 *
 * What we can verify from the client side is correctness, not the absence of
 * a copy: data written through one path must be visible through the other, the
 * buf_off must be honoured end to end, and short/EOF reads must report the
 * real byte count. We also pin the regression that pfsd_pread_zc used to
 * abort() on any server-side read error instead of returning errno.
 *
 * Reuses the shared gtest env (pfsd_testenv/pfsd_unittest) which mounts RW
 * before the cases run.
 * Usage: pfsd_zc_test <hostid> <cluster> <pbdname>
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pfsd_testenv.h"
#include "pfsd_sdk.h"

using std::string;

namespace
{

/* Deterministic, position-dependent pattern so a misplaced byte is caught. */
void
fill_pattern(void *buf, size_t len, uint8_t seed)
{
	auto *p = static_cast<uint8_t *>(buf);
	for (size_t i = 0; i < len; i++)
		p[i] = static_cast<uint8_t>((i * 31u + seed) & 0xff);
}

/*
 * Sector granule for the underlying device. A zero-copy IO whose buf_off, len,
 * and file offset are all sector-aligned can be DMA'd straight out of the
 * caller's buffer. When any of them is misaligned the daemon does NOT reject
 * it -- it transparently copies through its own sector-aligned staging buffer,
 * so the IO still succeeds and the bytes still round-trip; it just isn't
 * zero-copy for that call. The tests below drive deliberately-misaligned
 * operands to pin that copy-fallback behaviour.
 */
constexpr size_t kSectorSize = 4096;

bool
is_aligned(uint64_t v, size_t a)
{
	return (v & (a - 1)) == 0;
}

/*
 * One caller-owned, fd-backed buffer registered for the whole suite. This is
 * the WAL-buffer model: the client owns the memory, hands pfsd a buffer id
 * once, and every zero-copy IO addresses a slice of it by (id, offset).
 * Registration must happen before mount, so it lives in SetUpTestSuite.
 */
constexpr size_t kForeignBufSize = 256 * 1024;
constexpr int kZcMountFlags = PFS_RDWR;  /* mount() adds PFS_TOOL */
int g_memfd = -1;
uint8_t *g_base = nullptr;
int64_t g_buf_id = -1;

class ZcTest : public testing::Test {
public:
	/* Register the foreign buffer once, before (re)mounting the suite. */
	static void SetUpTestSuite()
	{
		ASSERT_EQ(0, g_testenv->umount()) << "umount before register";

		g_memfd = memfd_create("pg-wal-buf", MFD_ALLOW_SEALING);
		ASSERT_GE(g_memfd, 0) << strerror(errno);
		ASSERT_EQ(0, ftruncate(g_memfd, kForeignBufSize)) << strerror(errno);
		g_base = static_cast<uint8_t *>(mmap(nullptr, kForeignBufSize,
		    PROT_READ | PROT_WRITE, MAP_SHARED, g_memfd, 0));
		ASSERT_NE(MAP_FAILED, static_cast<void *>(g_base)) << strerror(errno);

		g_buf_id = pfsd_register_shared_buffer(g_memfd, kForeignBufSize);
		ASSERT_GE(g_buf_id, 0) << "register failed: " << strerror(errno);

		ASSERT_EQ(0, g_testenv->mount(kZcMountFlags))
		    << "remount after register";
	}

	/* Drop the buffer and restore the plain mount the shared env expects. */
	static void TearDownTestSuite()
	{
		if (g_base && g_base != MAP_FAILED)
			munmap(g_base, kForeignBufSize);
		if (g_memfd >= 0)
			close(g_memfd);
		g_base = nullptr;
		g_memfd = -1;
		g_buf_id = -1;
		g_testenv->umount();
		g_testenv->mount(kZcMountFlags);
	}

	void SetUp() override
	{
		used_ = 0;
		pbdpath_ = "/" + g_testenv->pbdname_ + "/zc.txt";
		fd_ = pfsd_creat(pbdpath_.data(), 0);
		ASSERT_GE(fd_, 0) << strerror(errno);
	}

	void TearDown() override
	{
		if (fd_ >= 0)
			pfsd_close(fd_);
		pfsd_unlink(pbdpath_.data());
	}

	/*
	 * Bump-allocate a 4 KiB-aligned slice of the registered buffer. Replaces
	 * pfsd_alloc for these tests: same {buf_id, offset, ptr} shape, but the
	 * memory is the caller's registered buffer rather than a PFS pool. The
	 * bump cursor resets each test (SetUp), so there is nothing to free.
	 */
	struct Slice { uint64_t buf_id; off_t offs; uint8_t *ptr; };
	Slice slice(size_t len)
	{
		off_t off = static_cast<off_t>(used_);
		used_ += (len + 4095) & ~size_t(4095);
		EXPECT_LE(used_, kForeignBufSize) << "test buffer exhausted";
		return Slice{ static_cast<uint64_t>(g_buf_id), off, g_base + off };
	}

	string pbdpath_;
	int fd_ = -1;
	size_t used_ = 0;
};

} // namespace

/* pfsd_alloc must hand back a usable slice, and pfsd_free must take it back. */
TEST_F(ZcTest, AllocFree)
{
	pfsd_buf buf = pfsd_alloc(4096);
	ASSERT_NE(buf.ptr, nullptr) << "pfsd_alloc returned null";
	EXPECT_NE(buf.buf_id, static_cast<uint64_t>(-1));
	EXPECT_GE(buf.offs, 0);

	/* Slice is writable by us before/after the daemon touches it. */
	std::memset(buf.ptr, 0xab, 4096);

	pfsd_free(buf);
}

/* Write a slice in place, read it back into another slice, bytes must match. */
TEST_F(ZcTest, WriteThenReadRoundTrip)
{
	const size_t len = 8192;

	Slice wb = slice(len);
	fill_pattern(wb.ptr, len, 0x5a);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, wb.buf_id, wb.offs, len, 0))
	    << strerror(errno);

	Slice rb = slice(len);
	std::memset(rb.ptr, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, rb.buf_id, rb.offs, len, 0))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(wb.ptr, rb.ptr, len));
}

/* A zero-copy write must be visible to an ordinary (copying) read. */
TEST_F(ZcTest, ZcWriteVisibleToNormalRead)
{
	const size_t len = 4096;

	Slice wb = slice(len);
	fill_pattern(wb.ptr, len, 0x11);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, wb.buf_id, wb.offs, len, 0))
	    << strerror(errno);

	std::vector<uint8_t> local(len, 0);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread(fd_, local.data(), len, 0))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(local.data(), wb.ptr, len));
}

/* An ordinary (copying) write must be visible to a zero-copy read. */
TEST_F(ZcTest, NormalWriteVisibleToZcRead)
{
	const size_t len = 4096;

	std::vector<uint8_t> local(len);
	fill_pattern(local.data(), len, 0x22);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite(fd_, local.data(), len, 0))
	    << strerror(errno);

	Slice rb = slice(len);
	std::memset(rb.ptr, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, rb.buf_id, rb.offs, len, 0))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(rb.ptr, local.data(), len));
}

/*
 * The IO can target a sub-range of a larger buffer; buf_off must be honoured
 * on both the write and the read so the daemon touches exactly that slice.
 */
TEST_F(ZcTest, SubBufferOffsetHonoured)
{
	const size_t cap = 16384;
	const size_t len = 4096;
	const off_t woff = 4096;   /* payload at [4096, 8192)                  */
	const off_t roff = 12288;  /* read back into [12288, 16384), leaving a */
	                           /* poison gap [8192, 12288) in between       */

	Slice buf = slice(cap);
	auto *base = buf.ptr;

	std::memset(base, 0xee, cap);          /* poison the whole buffer */
	fill_pattern(base + woff, len, 0x33);  /* real payload in the middle */

	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, buf.buf_id, buf.offs + woff, len, 0))
	    << strerror(errno);

	std::memset(base + roff, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, buf.buf_id, buf.offs + roff, len, 0))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(base + woff, base + roff, len));
	/* The poison bytes just before the read range must be untouched. */
	EXPECT_EQ(0xee, base[roff - 1]);
}

/*
 * A misaligned buf_off (the DMA source pointer) must not break the IO: the
 * daemon copies through a sector-aligned staging buffer instead of DMAing in
 * place, so the round-trip still returns the full count and the exact bytes.
 */
TEST_F(ZcTest, UnalignedBufOffsetCopiesAndRoundTrips)
{
	const size_t len = 8192;
	const off_t skew = 37;   /* odd, deliberately sub-sector */

	/* Headroom so the skewed slice cannot overrun the reserved region. */
	Slice wb = slice(len + kSectorSize);
	Slice rb = slice(len + kSectorSize);
	off_t woff = wb.offs + skew;
	off_t roff = rb.offs + skew;
	ASSERT_FALSE(is_aligned(static_cast<uint64_t>(woff), kSectorSize));
	ASSERT_FALSE(is_aligned(static_cast<uint64_t>(roff), kSectorSize));

	fill_pattern(g_base + woff, len, 0x5b);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, wb.buf_id, woff, len, 0))
	    << strerror(errno);

	std::memset(g_base + roff, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, rb.buf_id, roff, len, 0))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(g_base + woff, g_base + roff, len));
}

/*
 * A misaligned length is handled the same way (copy fallback): the write and
 * the matching short read both honour the exact, non-sector-multiple count.
 */
TEST_F(ZcTest, UnalignedLengthCopiesAndRoundTrips)
{
	const size_t len = 5000;   /* not a multiple of the sector size */
	ASSERT_FALSE(is_aligned(len, kSectorSize));

	Slice wb = slice(len);
	fill_pattern(wb.ptr, len, 0x6c);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, wb.buf_id, wb.offs, len, 0))
	    << strerror(errno);

	Slice rb = slice(len);
	std::memset(rb.ptr, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, rb.buf_id, rb.offs, len, 0))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(wb.ptr, rb.ptr, len));
}

/*
 * A misaligned *file* offset likewise copies rather than failing: data written
 * at a sub-sector file position reads back byte-for-byte from that position.
 */
TEST_F(ZcTest, UnalignedFileOffsetCopiesAndRoundTrips)
{
	const size_t len = 4096;
	const off_t foff = 100;   /* sub-sector file offset */
	ASSERT_FALSE(is_aligned(static_cast<uint64_t>(foff), kSectorSize));

	Slice wb = slice(len);
	fill_pattern(wb.ptr, len, 0x7d);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, wb.buf_id, wb.offs, len, foff))
	    << strerror(errno);

	Slice rb = slice(len);
	std::memset(rb.ptr, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, rb.buf_id, rb.offs, len, foff))
	    << strerror(errno);

	EXPECT_EQ(0, std::memcmp(wb.ptr, rb.ptr, len));
}

/* A read that runs into EOF must report the real (short) count, not error. */
TEST_F(ZcTest, ShortReadAtEof)
{
	const size_t len = 4096;
	const size_t tail = 1024;

	Slice wb = slice(len);
	fill_pattern(wb.ptr, len, 0x44);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, wb.buf_id, wb.offs, len, 0))
	    << strerror(errno);

	Slice rb = slice(len);

	/* Start `tail` bytes before EOF, ask for more than remains. */
	std::memset(rb.ptr, 0, len);
	ssize_t n = pfsd_pread_zc(fd_, rb.buf_id, rb.offs, len,
	    static_cast<off_t>(len - tail));
	EXPECT_EQ(static_cast<ssize_t>(tail), n) << strerror(errno);
	EXPECT_EQ(0, std::memcmp(rb.ptr, wb.ptr + (len - tail), tail));

	/* Fully past EOF: zero bytes, no error. */
	n = pfsd_pread_zc(fd_, rb.buf_id, rb.offs, len,
	    static_cast<off_t>(len + 4096));
	EXPECT_EQ(0, n) << strerror(errno);
}

/*
 * Regression: pfsd_pread_zc used to abort() the whole process on any
 * server-side read error. A read on a directory fd returns EISDIR; the call
 * must now fail cleanly with -1/errno and leave the process standing.
 */
TEST_F(ZcTest, ReadErrorReturnsErrnoNotAbort)
{
	string dirpath = "/" + g_testenv->pbdname_ + "/";
	int dfd = pfsd_open(dirpath.c_str(), 0, 0666);
	ASSERT_GE(dfd, 0) << strerror(errno);

	const size_t len = 4096;
	Slice rb = slice(len);

	errno = 0;
	ssize_t n = pfsd_pread_zc(dfd, rb.buf_id, rb.offs, len, 0);
	EXPECT_EQ(-1, n);
	EXPECT_EQ(EISDIR, errno);

	pfsd_close(dfd);
}

/*
 * Safety: a client-supplied range running past the end of the registered
 * buffer must be rejected by the daemon's bounds check (EINVAL), not blindly
 * dereferenced. The offset is valid but offset+len overruns the buffer.
 */
TEST_F(ZcTest, OutOfRangeOffsetRejected)
{
	const size_t len = 8192;

	fill_pattern(g_base, 4096, 0x66);
	ASSERT_EQ(4096, pfsd_pwrite_zc(fd_, static_cast<uint64_t>(g_buf_id),
	    0, 4096, 0)) << strerror(errno);

	errno = 0;
	ssize_t n = pfsd_pwrite_zc(fd_, static_cast<uint64_t>(g_buf_id),
	    static_cast<off_t>(kForeignBufSize - 1024), len, 0);
	EXPECT_EQ(-1, n);
	EXPECT_EQ(EINVAL, errno);
}

/*
 * Create a fresh memfd-backed buffer, register it with the *already mounted*
 * SDK, and return its id (or -1). Used by the post-mount register/unregister
 * cases below. On success base_out and fd_out receive the mapping and fd; the
 * caller cleans them up.
 */
static int64_t
register_runtime_buffer(size_t size, uint8_t **base_out, int *fd_out)
{
	int mfd = memfd_create("zc-runtime-buf", MFD_ALLOW_SEALING);
	if (mfd < 0)
		return -1;
	if (ftruncate(mfd, size) != 0) {
		close(mfd);
		return -1;
	}
	auto *base = static_cast<uint8_t *>(mmap(nullptr, size,
	    PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0));
	if (base == MAP_FAILED) {
		close(mfd);
		return -1;
	}
	int64_t id = pfsd_register_shared_buffer(mfd, size);
	if (id < 0) {
		munmap(base, size);
		close(mfd);
		return -1;
	}
	*base_out = base;
	*fd_out = mfd;
	return id;
}

/*
 * The point of the feature: a buffer registered AFTER mount must serve
 * zero-copy IO just like a pre-mount one, and unregistering it must make the
 * daemon reject further IO against that id (EINVAL, not a crash).
 */
TEST_F(ZcTest, RegisterAfterMountThenUnregister)
{
	const size_t bufsz = 64 * 1024;
	uint8_t *base = nullptr;
	int mfd = -1;
	int64_t id = register_runtime_buffer(bufsz, &base, &mfd);
	ASSERT_GE(id, 0) << "post-mount register failed: " << strerror(errno);

	/* Round-trip through the freshly registered buffer. */
	const size_t len = 8192;
	fill_pattern(base, len, 0x77);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, static_cast<uint64_t>(id), 0, len, 0))
	    << strerror(errno);

	std::memset(base + len, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, static_cast<uint64_t>(id), len, len, 0))
	    << strerror(errno);
	EXPECT_EQ(0, std::memcmp(base, base + len, len));

	/* After unregister the daemon no longer knows the id. */
	ASSERT_EQ(0, pfsd_unregister_shared_buffer(id)) << strerror(errno);

	errno = 0;
	ssize_t n = pfsd_pwrite_zc(fd_, static_cast<uint64_t>(id), 0, len, 0);
	EXPECT_EQ(-1, n);
	EXPECT_EQ(EINVAL, errno);

	munmap(base, bufsz);
	close(mfd);
}

/*
 * Registering again after an unregister must yield a working buffer with a
 * fresh, non-reused id.
 */
TEST_F(ZcTest, ReRegisterAfterUnregister)
{
	const size_t bufsz = 32 * 1024;

	uint8_t *base1 = nullptr;
	int mfd1 = -1;
	int64_t id1 = register_runtime_buffer(bufsz, &base1, &mfd1);
	ASSERT_GE(id1, 0) << strerror(errno);
	ASSERT_EQ(0, pfsd_unregister_shared_buffer(id1)) << strerror(errno);
	munmap(base1, bufsz);
	close(mfd1);

	uint8_t *base2 = nullptr;
	int mfd2 = -1;
	int64_t id2 = register_runtime_buffer(bufsz, &base2, &mfd2);
	ASSERT_GE(id2, 0) << strerror(errno);
	EXPECT_NE(id1, id2) << "buffer ids must not be reused";

	const size_t len = 4096;
	fill_pattern(base2, len, 0x88);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pwrite_zc(fd_, static_cast<uint64_t>(id2), 0, len, 0))
	    << strerror(errno);
	std::memset(base2 + len, 0, len);
	ASSERT_EQ(static_cast<ssize_t>(len),
	    pfsd_pread_zc(fd_, static_cast<uint64_t>(id2), len, len, 0))
	    << strerror(errno);
	EXPECT_EQ(0, std::memcmp(base2, base2 + len, len));

	EXPECT_EQ(0, pfsd_unregister_shared_buffer(id2)) << strerror(errno);
	munmap(base2, bufsz);
	close(mfd2);
}
