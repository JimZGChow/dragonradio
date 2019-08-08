#ifndef HEADER_HH_
#define HEADER_HH_

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <netinet/in.h>

#include <complex>
#include <cstddef>
#include <iterator>
#include <vector>

#include <liquid/liquid.h>

#include "Seq.hh"

typedef uint8_t NodeId;

/** @brief %PHY packet header. */
struct Header {
    /** @brief Current hop. */
    NodeId curhop;

    /** @brief Next hop. */
    NodeId nexthop;

    /** @brief Packet sequence number. */
    Seq seq;

    /** @brief Packet flags. */
    struct {
        /** @brief Set if the packet is the first in a new connection */
        uint8_t syn : 1;

        /** @brief Set if the packet is ACKing */
        uint8_t ack : 1;

        /** @brief Set if this is a broadcast packet */
        uint8_t broadcast : 1;

        /** @brief Set if the packet has data */
        uint8_t has_data : 1;

        /** @brief Set if the packet has control data */
        uint8_t has_control : 1;

        /** @brief Unused flags */
        uint8_t unused : 3;
    } flags;
} __attribute__((packed));

/** @brief Extended header that appears in radio payload. */
struct ExtendedHeader {
    /** @brief Source */
    NodeId src;

    /** @brief Destination */
    NodeId dest;

    /** @brief Sequence number we are ACK'ing or NAK'ing. */
    Seq ack;

    /** @brief Length of the packet payload. */
    /** The packet payload may be padded or contain control data. This field
     * gives the size of the data portion of the payload.
     */
    uint16_t data_len;
};

#endif /* HEADER_HH_ */
