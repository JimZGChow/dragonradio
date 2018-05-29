#ifndef PACKET_HH_
#define PACKET_HH_

#include <sys/types.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include <liquid/liquid.h>

#include "buffer.hh"

typedef uint8_t NodeId;

enum {
    /** @brief Set if the packet is ACKing */
    kACK = 0,

    /** @brief Set if the packet is NAKing */
    kNAK
};

typedef uint16_t PacketFlags;

struct Seq {
    using uint_type = uint16_t;

    Seq() = default;
    Seq(uint_type seq) : seq_(seq) {};

    Seq(const Seq&) = default;
    Seq(Seq&&) = default;

    Seq& operator=(const Seq&) = default;
    Seq& operator=(Seq&&) = default;

    bool operator ==(const Seq& other) { return seq_ == other.seq_; }
    bool operator !=(const Seq& other) { return seq_ != other.seq_; }

    bool operator <(const Seq& other)
      { return static_cast<int16_t>(seq_ - other.seq_) < 0; }

    bool operator <=(const Seq& other)
      { return static_cast<int16_t>(seq_ - other.seq_) <= 0; }

    bool operator >(const Seq& other)
      { return static_cast<int16_t>(seq_ - other.seq_) > 0; }

    bool operator >=(const Seq& other)
      { return static_cast<int16_t>(seq_ - other.seq_) >= 0; }

    Seq operator ++() { seq_++; return *this; }
    Seq operator ++(int) { seq_++; return seq_ - 1; }

    Seq operator --() { seq_--; return *this; }
    Seq operator --(int) { seq_--; return seq_ + 1; }

    Seq operator +(int i) { return seq_ + i; }
    Seq operator -(int i) { return seq_ - i; }

    operator uint_type() const { return seq_; }

    uint_type seq_;
};

/** @brief %PHY packet header. */
struct Header {
    /** @brief Current hop. */
    NodeId curhop;

    /** @brief Next hop. */
    NodeId nexthop;

    /** @brief Packet flags. */
    PacketFlags flags;

    /** @brief Packet sequence number. */
    Seq seq;

    /** @brief Length of the packet payload. */
    /** The packet payload may be padded or contain control data. This field
     * gives the size of the data portion of the payload.
     */
    uint16_t data_len;
};

/** @brief Extened header that appears in radio payload. */
struct ExtendedHeader {
    /** @brief Source */
    NodeId src;

    /** @brief Destination */
    NodeId dest;

    /** @brief Sequence number we are ACK'ing or NAK'ing. */
    Seq ack;
};

/** @brief A packet. */
struct Packet : public buffer<unsigned char>
{
    Packet() : buffer() {};
    Packet(size_t n) : buffer(n) {};
    Packet(unsigned char* data, size_t n) : buffer(data, n) {}

    /** @brief Current hop */
    /** If the packet originated in the network, this should be the current
     * node.
     */
    NodeId curhop;

    /** @brief Next hop */
    /** If the packet originated from the radio, this should be the current
     * node.
     */
    NodeId nexthop;

    /** @brief Packet flags. */
    PacketFlags flags;

    /** @brief Sequence number */
    Seq seq;

    /** @brief Length of data portion of the packet */
    uint16_t data_len;

    /** @brief Source */
    NodeId src;

    /** @brief Destination */
    NodeId dest;

    /** @brief Get extended header */
    ExtendedHeader &getExtendedHeader(void)
    {
        return *reinterpret_cast<ExtendedHeader*>(data());
    }

    /** @brief Copy internal values to a PHY header */
    void toHeader(Header &hdr)
    {
        ExtendedHeader &ehdr = getExtendedHeader();

        hdr.curhop = curhop;
        hdr.nexthop = nexthop;
        hdr.flags = flags;
        hdr.seq = seq;
        hdr.data_len = data_len;

        ehdr.src = src;
        ehdr.dest = dest;
    }

    /** @brief Copy values from PHY header to packet */
    void fromHeader(Header &hdr)
    {
        ExtendedHeader &ehdr = getExtendedHeader();

        curhop = hdr.curhop;
        nexthop = hdr.nexthop;
        flags = hdr.flags;
        seq = hdr.seq;
        data_len = std::min(hdr.data_len, static_cast<uint16_t>(sizeof(ExtendedHeader) + size()));

        src = ehdr.src;
        dest = ehdr.dest;
    }
};

/** @brief A packet received from the network. */
struct NetPacket : public Packet
{
    NetPacket(size_t n) : Packet(n) {};

    /** @brief CRC */
    crc_scheme check;

    /** @brief FEC0 (inner FEC) */
    fec_scheme fec0;

    /** @brief FEC1 (outer FEC) */
    fec_scheme fec1;

    /** @brief Modulation scheme */
    modulation_scheme ms;

    /** @brief Soft TX gain */
    float g;
};

/** @brief A packet received from the radio. */
struct RadioPacket : public Packet
{
    RadioPacket() : Packet(), delivered(false), barrier(false) {};
    RadioPacket(unsigned char* data, size_t n) : Packet(data, n), delivered(false), barrier(false) {}

    /** @brief Error vector magnitude [dB] */
    float evm;

    /** @brief Received signal strength indicator [dB] */
    float rssi;

    /** @brief This flag is set if the packet has been delivered. */
    bool delivered;

    /** @brief This Boolean is true if this packet is a barrier and should not
     * be processed or removed from a queue except by its creator.
     */
    bool barrier;
};

#endif /* PACKET_HH_ */
