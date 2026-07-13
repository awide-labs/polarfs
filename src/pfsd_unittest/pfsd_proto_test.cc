/*
 * pfsd_proto_test — wire-compatibility of the handshake capability bitmap.
 *
 * The CLIENT_HELLO / SERVER_HELLO messages carry a trailing uint64 capability
 * bitmap appended after the pre-existing fields, with the message `version`
 * left at 1. These tests pin the two directions of backward compatibility that
 * make that safe:
 *
 *   - a new peer round-trips the bitmap intact;
 *   - a message produced the *old* way (no trailing caps, shorter totalLength)
 *     still deserializes with the current code and reports caps == 0; and
 *   - a new message stays length-framed so an old peer -- which reads only the
 *     pre-caps fields and then advances by totalLength -- stays in sync.
 *
 * Pure serialize/deserialize checks; no daemon or mount required.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ipc/proto.h"

using namespace ipc;

/* Serialize the way the pre-capability client did: no trailing caps field. */
static std::vector<uint8_t>
serialize_client_hello_v1(const std::string &cluster, const std::string &pbdname,
                          int host_id, int flags)
{
	const size_t totalSize =
	    sizeof(uint32_t) +                  // message type
	    sizeof(uint32_t) +                  // total length
	    sizeof(uint32_t) +                  // version
	    sizeof(uint32_t) + cluster.size() + // cluster
	    sizeof(uint32_t) + pbdname.size() + // pbdname
	    sizeof(int32_t) +                   // host_id
	    sizeof(int32_t);                    // flags

	std::vector<uint8_t> buf(totalSize);
	pfsutil::BufWriter w(buf.data(), buf.size());
	w.writeLE<uint32_t>(CLIENT_HELLO);
	w.writeLE<uint32_t>(totalSize);
	w.writeLE<uint32_t>(1); // version
	w.writeLE<uint32_t>(cluster.size());
	w.push(reinterpret_cast<const uint8_t *>(cluster.data()), cluster.size());
	w.writeLE<uint32_t>(pbdname.size());
	w.push(reinterpret_cast<const uint8_t *>(pbdname.data()), pbdname.size());
	w.writeLE<int32_t>(host_id);
	w.writeLE<int32_t>(flags);
	buf.resize(w.bytesWritten());
	return buf;
}

/* Serialize the way the pre-capability server did: no trailing caps field. */
static std::vector<uint8_t>
serialize_server_hello_v1(uint64_t connId, int error)
{
	const size_t totalSize = sizeof(uint32_t) + // message type
	                         sizeof(uint32_t) + // total length
	                         sizeof(uint32_t) + // version
	                         sizeof(uint64_t) + // connectionId
	                         sizeof(int32_t);   // error

	std::vector<uint8_t> buf(totalSize);
	pfsutil::BufWriter w(buf.data(), buf.size());
	w.writeLE<uint32_t>(SERVER_HELLO);
	w.writeLE<uint32_t>(totalSize);
	w.writeLE<uint32_t>(1); // version
	w.writeLE<uint64_t>(connId);
	w.writeLE<int32_t>(error);
	buf.resize(w.bytesWritten());
	return buf;
}

/* New client -> new server: the bitmap survives the round-trip untouched. */
TEST(ProtoCaps, ClientHelloRoundTrip)
{
	ClientHelloMessage out;
	out.cluster = "mycluster";
	out.pbdname = "mypbd";
	out.host_id = 3;
	out.flags = 0x5;
	out.caps = CAP_ZEROCOPY;

	auto bytes = out.serialize();

	ClientHelloMessage in;
	ASSERT_TRUE(in.deserialize(bytes.data(), bytes.size()));
	EXPECT_EQ("mycluster", in.cluster);
	EXPECT_EQ("mypbd", in.pbdname);
	EXPECT_EQ(3, in.host_id);
	EXPECT_EQ(0x5, in.flags);
	EXPECT_EQ(CAP_ZEROCOPY, in.caps);
}

TEST(ProtoCaps, ServerHelloRoundTrip)
{
	ServerHelloMessage out;
	out.connectionId = 0xdeadbeef;
	out.error = 0;
	out.caps = CAP_ZEROCOPY;

	auto bytes = out.serialize();

	ServerHelloMessage in;
	ASSERT_TRUE(in.deserialize(bytes.data(), bytes.size()));
	EXPECT_EQ(0xdeadbeefu, in.connectionId);
	EXPECT_EQ(0, in.error);
	EXPECT_EQ(CAP_ZEROCOPY, in.caps);
}

/* Old client wire (no trailing caps) must parse as caps == 0, not fail. */
TEST(ProtoCaps, OldClientHelloParsesAsNoCaps)
{
	auto bytes = serialize_client_hello_v1("clu", "pbd", 7, 0x11);

	ClientHelloMessage in;
	ASSERT_TRUE(in.deserialize(bytes.data(), bytes.size()));
	EXPECT_EQ("clu", in.cluster);
	EXPECT_EQ("pbd", in.pbdname);
	EXPECT_EQ(7, in.host_id);
	EXPECT_EQ(0x11, in.flags);
	EXPECT_EQ(0u, in.caps) << "absent bitmap must read as no capabilities";
}

/* Old server wire (no trailing caps) must parse as caps == 0, not fail. */
TEST(ProtoCaps, OldServerHelloParsesAsNoCaps)
{
	auto bytes = serialize_server_hello_v1(0x1234, 0);

	ServerHelloMessage in;
	ASSERT_TRUE(in.deserialize(bytes.data(), bytes.size()));
	EXPECT_EQ(0x1234u, in.connectionId);
	EXPECT_EQ(0, in.error);
	EXPECT_EQ(0u, in.caps);
}

/*
 * An old reader frames by the declared totalLength and reads only the pre-caps
 * fields. Verify the new message's header length equals its byte count and
 * that exactly one uint64 (the caps) trails the old fields -- so an old peer
 * consumes the whole message and stays in sync, silently dropping the bitmap.
 */
TEST(ProtoCaps, NewClientHelloStaysLengthFramedForOldReader)
{
	ClientHelloMessage out;
	out.cluster = "clu";
	out.pbdname = "pbd";
	out.host_id = 1;
	out.flags = 0;
	out.caps = CAP_ZEROCOPY;
	auto bytes = out.serialize();

	pfsutil::BufReader r(bytes.data(), bytes.size());
	EXPECT_EQ(static_cast<uint32_t>(CLIENT_HELLO), r.readLE<uint32_t>());
	const uint32_t totalLen = r.readLE<uint32_t>();
	EXPECT_EQ(bytes.size(), totalLen) << "header length must match byte count";

	/* Walk the pre-caps fields exactly as an old reader would. */
	EXPECT_EQ(1u, r.readLE<uint32_t>()); // version
	r.readFixedString(r.readLE<uint32_t>()); // cluster
	r.readFixedString(r.readLE<uint32_t>()); // pbdname
	r.readLE<int32_t>(); // host_id
	r.readLE<int32_t>(); // flags

	EXPECT_EQ(sizeof(uint64_t), totalLen - r.position())
	    << "exactly the caps bitmap trails the v1 fields";
}

int
main(int argc, char **argv)
{
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
