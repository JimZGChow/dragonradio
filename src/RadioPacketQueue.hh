#ifndef RADIOPACKETQUEUE_H_
#define RADIOPACKETQUEUE_H_

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>

#include "IQBuffer.hh"
#include "Packet.hh"

/** @brief A thread-safe queue of network packets. Handles barriers. */
/** This is a specialized queue for RadioPacket@s that handles barrier packets. A
 * barrier packet is a special packet that will not be removed from a queue---
 * seeing a barrier packet is like seeign the end of the queue. Barriers allow
 * proper ordering: a producer can insert a barrier, insert packets before the
 * barrier, then remove the barrier when it is done producing, thereby
 * guaranteeing that packets inserted *after* the barrier will not be read from
 * the queue until the barrier has been removed.
 */
class RadioPacketQueue {
public:
    /** @brief A barrier */
    using barrier = std::list<std::unique_ptr<RadioPacket>>::iterator;

    RadioPacketQueue();
    ~RadioPacketQueue();

    RadioPacketQueue(const RadioPacketQueue&) = delete;
    RadioPacketQueue(RadioPacketQueue&&) = delete;

    RadioPacketQueue& operator=(const RadioPacketQueue&) = delete;
    RadioPacketQueue& operator=(RadioPacketQueue&&) = delete;

    /** @brief Add a RadioPacket to the queue.
     * @param pkt The packet to push onto the queue.
     */
    void push(std::unique_ptr<RadioPacket> pkt);

    /** @brief Add a RadioPacket to the queue before a barrier.
     * @param b The barrier.
     * @param pkt The packet to push onto the queue.
     */
    void push(barrier b, std::unique_ptr<RadioPacket> pkt);

    /** @brief Push a barrier onto the queue.
     * @return A barrier.
     */
    barrier push_barrier(void);

    /** @brief Erase a barrier from the queue.
     * @param The barrier.
     */
    void erase_barrier(barrier b);

    /** @brief Get a RadioPacket from the queue.
     * @param pkt The popped packet.
     * @return Return true if pop was successful, false otherwise.
     */
    bool pop(std::unique_ptr<RadioPacket>& pkt);

    /** @brief Stop processing this queue.*/
    void stop(void);

private:
    /** @brief Flag that is true when we should finish processing. */
    bool _done;

    /** @brief Mutex protecting the queue of packets. */
    std::mutex _m;

    /** @brief Condition variable protecting the queue of packets. */
    std::condition_variable _cond;

    /** @brief The number of items in the queue of packets. */
    size_t _size;

    /** @brief The queue of packets. */
    std::list<std::unique_ptr<RadioPacket>> _q;
};


#endif /* RADIOPACKETQUEUE_H_ */
