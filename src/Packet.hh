#ifndef PACKET_HH_
#define PACKET_HH_

#include <sys/types.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "buffer.hh"
#include "Node.hh"

typedef uint16_t PacketId;

/** @brief A packet recevied from the network. */
struct NetPacket : public buffer<unsigned char>
{
    NetPacket(size_t n) : buffer(n) {};

    /** @brief Packet ID */
    PacketId pkt_id;

    /** @brief Source node (should be this node) */
    NodeId src;

    /** @brief Destination node */
    NodeId dest;
};

/** @brief A packet received from the radio. */
struct RadioPacket : public buffer<unsigned char>
{
    RadioPacket() : buffer(), barrier(false) {};

    RadioPacket(unsigned char* data, size_t n) : buffer(data, n), barrier(false) {}

    /** @brief Packet ID */
    PacketId pkt_id;

    /** @brief Source node */
    NodeId src;

    /** @brief Destination node (should be this node) */
    NodeId dest;

    /** @brief This Boolean is true if this packet is a barrier and should not
     * be processed or removed from a queue except by its creator.
     */
    bool barrier;
};

#endif /* PACKET_HH_ */
