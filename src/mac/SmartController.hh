#ifndef SMARTCONTROLLER_H_
#define SMARTCONTROLLER_H_

#include <sys/types.h>
#include <netinet/if_ether.h>

#include <list>
#include <map>
#include <random>

#include "heap.hh"
#include "spinlock_mutex.hh"
#include "Clock.hh"
#include "TimerQueue.hh"
#include "TimeSync.hh"
#include "net/Queue.hh"
#include "mac/Controller.hh"
#include "mac/MAC.hh"
#include "phy/PHY.hh"

class SmartController;

struct SendWindow {
    struct Entry : public TimerQueue::Timer {
        Entry(SendWindow &sendw) : sendw(sendw), pkt(nullptr) {};

        virtual ~Entry() = default;

        void operator =(std::shared_ptr<NetPacket>& p)
        {
            pkt = p;
        }

        operator bool()
        {
            return (bool) pkt;
        }

        operator std::shared_ptr<NetPacket>()
        {
            return pkt;
        }

        void reset(void)
        {
            pkt.reset();
        }

        void operator()() override;

        /** @brief The send window. */
        SendWindow &sendw;

        /** @brief The packet received in this window entry. */
        std::shared_ptr<NetPacket> pkt;
    };

    using vector_type = std::vector<Entry>;

    SendWindow(NodeId node_id, SmartController &controller, Seq::uint_type maxwin)
      : node_id(node_id)
      , controller(controller)
      , unack(0)
      , max(0)
      , new_window(true)
      , win(1)
      , maxwin(maxwin)
      , mcsidx(0)
      , mcsidx_prob(0)
      , entries_(maxwin, *this)
    {
    }

    /** @brief Node ID of destination. */
    NodeId node_id;

    /** @brief Our controller. */
    SmartController &controller;

    /** @brief First un-ACKed sequence number. */
    std::atomic<Seq> unack;

    /** @brief Maximum sequence number we have sent. */
    /** INVARIANT: max < unack + win */
    std::atomic<Seq> max;

    /** @brief Is this a new window? */
    bool new_window;

    /** @brief Send window size */
    Seq::uint_type win;

    /** @brief Maximum window size */
    Seq::uint_type maxwin;

    /** @brief Modulation index */
    size_t mcsidx;

    /** @brief First sequence number at this modulation index */
    Seq mcsidx_init_seq;

    /** @brief The probability of moving to a given MCS */
    std::vector<double> mcsidx_prob;

    /** @brief Pending packets we can't send because our window isn't large enough */
    std::list<std::shared_ptr<NetPacket>> pending;

    /** @brief Mutex for the send window */
    spinlock_mutex mutex;

    /** @brief Return the packet with the given sequence number in the window */
    Entry& operator[](Seq seq)
    {
        return entries_[seq % entries_.size()];
    }

private:
    /** @brief Unacknowledged packets in our send window. */
    /** INVARIANT: unack <= N <= max < unack + win */
    vector_type entries_;
};

struct RecvWindow : public TimerQueue::Timer  {
    struct Entry {
        Entry() : received(false), delivered(false), pkt(nullptr) {};

        void operator =(std::shared_ptr<RadioPacket>&& p)
        {
            received = true;
            delivered = false;
            pkt = std::move(p);
        }

        void alreadyDelivered(void)
        {
            received = true;
            delivered = true;
        }

        void reset(void)
        {
            received = false;
            delivered = false;
            pkt.reset();
        }

        /** @brief Was this entry in the window received? */
        bool received;

        /** @brief Was this entry in the window delivered? */
        bool delivered;

        /** @brief The packet received in this window entry. */
        std::shared_ptr<RadioPacket> pkt;
    };

    using vector_type = std::vector<Entry>;

    RecvWindow(NodeId node_id,
               SmartController &controller,
               Seq::uint_type win,
               size_t nak_win)
      : node_id(node_id)
      , controller(controller)
      , ack(0)
      , max(0)
      , win(win)
      , explicit_nak_win(nak_win)
      , explicit_nak_idx(0)
      , entries_(win)
    {}

    /** @brief Node ID of destination. */
    NodeId node_id;

    /** @brief Our controller. */
    SmartController &controller;

    /** @brief Next sequence number we should ACK. */
    /** We have received (or given up) on all packets with sequence numbers <
     * this number. INVARIANT: The smallest sequence number in our receive
     * window must be STRICTLY > ack because we have already received the packet
     * with sequence number ack - 1, and if we received the packet with
     * sequence number ack, we should have updated ack to ack + 1.
     */
    Seq ack;

    /** @brief Maximum sequence number we have received */
    /** INVARIANT: ack <= max < ack + win. When max == ack, we have no holes in
    * our receive, window, which should therefore be empty, and we should ACK
    * ack+1. Otherwise we have a hole in our receive window and we should NAK
    * ack+1. Note than a NAK of sequence number N+1 implicitly ACKs N, since
    * otherwise we would've NAK'ed N instead.
    */
    Seq max;

    /** @brief Timestamp of packet with the maximum sequence number we have
      * sent. */
    Clock::time_point max_timestamp;

    /** @brief Receive window size */
    Seq::uint_type win;

    /** @brief Explicit NAK window */
    std::vector<MonoClock::time_point> explicit_nak_win;

    /** @brief Explicit NAK window index */
    size_t explicit_nak_idx;

    /** @brief Mutex for the receive window */
    spinlock_mutex mutex;

    /** @brief Return the packet with the given sequence number in the window */
    Entry& operator[](Seq seq)
    {
        return entries_[seq % entries_.size()];
    }

    void operator()() override;

private:
    /** @brief All packets with sequence numbers N such that
     * ack <= N <= max < ack + win
     */
    vector_type entries_;
};

/** @brief A MAC controller that implements ARQ. */
class SmartController : public Controller
{
public:
    SmartController(std::shared_ptr<Net> net,
                    std::shared_ptr<PHY> phy,
                    Seq::uint_type max_sendwin,
                    Seq::uint_type recvwin,
                    unsigned mcsidx_init,
                    double mcsidx_up_per_threshold,
                    double mcsidx_down_per_threshold,
                    double mcsidx_alpha,
                    double mcsidx_prob_floor);
    virtual ~SmartController();

    bool pull(std::shared_ptr<NetPacket>& pkt) override;

    void received(std::shared_ptr<RadioPacket>&& pkt) override;

    /** @brief Retransmit a send window entry on timeout. */
    void retransmitOnTimeout(SendWindow::Entry &entry);

    /** @brief Send an ACK to the given receiver. The caller MUST hold the lock
     * on recvw.
     */
    void ack(RecvWindow &recvw);

    /** @brief Send a NAK to the given receiver. */
    void nak(NodeId node_id, Seq seq);

    /** @brief Broadcast a HELLO packet. */
    void broadcastHello(void);

    /** @brief Get the controller's network queue. */
    std::shared_ptr<NetQueue> getNetQueue(void)
    {
        return netq_;
    }

    /** @brief Set the controller's network queue. */
    void setNetQueue(std::shared_ptr<NetQueue> q)
    {
        netq_ = q;
    }

    /** @brief Get the controller's MAC. */
    std::shared_ptr<MAC> getMAC(void)
    {
        return mac_;
    }

    /** @brief Set the controller's MAC. */
    void setMAC(std::shared_ptr<MAC> mac)
    {
        mac_ = mac;
    }

    /** @brief Get number of samples in a trnsmission slot */
    size_t getSlotSize(void)
    {
        return slot_size_;
    }

    /** @brief Set number of samples in a trnsmission slot */
    void setSlotSize(size_t size)
    {
        slot_size_ = size;
    }

    /** @brief Get PER threshold for increasing modulation level */
    double getUpPERThreshold(void)
    {
        return mcsidx_up_per_threshold_;
    }

    /** @brief Set PER threshold for increasing modulation level */
    void setUpPERThreshold(double thresh)
    {
        mcsidx_up_per_threshold_ = thresh;
    }

    /** @brief Get PER threshold for decreasing modulation level */
    double getDownPERThreshold(void)
    {
        return mcsidx_down_per_threshold_;
    }

    /** @brief Set PER threshold for decreasing modulation level */
    void setDownPERThreshold(double thresh)
    {
        mcsidx_down_per_threshold_ = thresh;
    }

    /** @brief Get MCS index learning alpha */
    double getMCSLearningAlpha(void)
    {
        return mcsidx_alpha_;
    }

    /** @brief Set MCS index learning alpha */
    void setMCSLearningAlpha(double alpha)
    {
        mcsidx_alpha_ = alpha;
    }

    /** @brief Get MCS transition probability floor */
    double getMCSProbFloor(void)
    {
        return mcsidx_prob_floor_;
    }

    /** @brief Set MCS transition probability floor */
    void setMCSProbFloor(double p)
    {
        mcsidx_prob_floor_ = p;
    }

    /** @brief Return explicit NAK window size. */
    bool getExplicitNAKWindow(void)
    {
        return explicit_nak_win_;
    }

    /** @brief Set explicit NAK window size. */
    void setExplicitNAKWindow(size_t n)
    {
        explicit_nak_win_ = n;
    }

    /** @brief Return explicit NAK window duration. */
    double getExplicitNAKWindowDuration(void)
    {
        return explicit_nak_win_duration_;
    }

    /** @brief Set explicit NAK window duration. */
    void setExplicitNAKWindowDuration(double t)
    {
        explicit_nak_win_duration_ = t;
    }

    /** @brief Return whether or not we should send selective NAKs. */
    bool getSelectiveNAK(void)
    {
        return selective_nak_;
    }

    /** @brief Set whether or not we should send selective NAKs. */
    void setSelectiveNAK(bool nak)
    {
        selective_nak_ = nak;
    }

    /** @brief Return flag indicating whether or not demodulation queue enforces
     * packet order.
     */
    bool getEnforceOrdering(void)
    {
        return enforce_ordering_;
    }

    /** @brief Set whether or not demodulation queue enforces packet order. */
    void setEnforceOrdering(bool enforce)
    {
        enforce_ordering_ = enforce;
    }

    /** @brief Return maximum number of packets per slot.
     * @param p The TXParams uses for modulation
     * @returns The number of packets of maximum size that can fit in one slot
     *          with the given modulation scheme.
     */
    size_t getMaxPacketsPerSlot(const TXParams &p)
    {
        return slot_size_/phy_->modulated_size(p, rc.mtu + sizeof(struct ether_header));
    }

    /** @brief Broadcast TX params */
    TXParams broadcast_tx_params;

protected:
    /** @brief Our PHY. */
    std::shared_ptr<PHY> phy_;

    /** @brief Our MAC. */
    std::shared_ptr<MAC> mac_;

    /** @brief Network queue with high-priority sub-queue. */
    std::shared_ptr<NetQueue> netq_;

    /** @brief Maximum size of a send window */
    Seq::uint_type max_sendwin_;

    /** @brief Size of receive window */
    Seq::uint_type recvwin_;

    /** @brief Send windows */
    std::map<NodeId, SendWindow> send_;

    /** @brief Mutex for the send windows */
    spinlock_mutex send_mutex_;

    /** @brief Receive windows */
    std::map<NodeId, RecvWindow> recv_;

    /** @brief Mutex for the receive windows */
    spinlock_mutex recv_mutex_;

    /** @brief Timer queue */
    TimerQueue timer_queue_;

    /** @brief Number of samples in a transmission slot */
    size_t slot_size_;

    /** @brief Initial MCS index */
    unsigned mcsidx_init_;

    /** @brief PER threshold for increasing modulation level */
    double mcsidx_up_per_threshold_;

    /** @brief PER threshold for decreasing modulation level */
    double mcsidx_down_per_threshold_;

    /** @brief Multiplicative factor used when learning MCS transition
     * probabilities
     */
    double mcsidx_alpha_;

    /** @brief Minimum MCS transition probability */
    double mcsidx_prob_floor_;

    /** @brief Explicit NAK window */
    size_t explicit_nak_win_;

    /** @brief Explicit NAK window duration */
    double explicit_nak_win_duration_;

    /** @brief Should we send selective NAK packets? */
    bool selective_nak_;

    /** @brief Should packets always be output in the order they were actually
     * received?
     */
    bool enforce_ordering_;

    /** @brief Time sync information */
    struct TimeSync time_sync_;

    /** @brief Random number generator */
    std::mt19937 gen_;

    /** @brief Uniform 0-1 real distribution */
    std::uniform_real_distribution<double> dist_;

    /** @brief Re-transmit a send window entry. */
    void retransmit(SendWindow::Entry &entry);

    /** @brief Start the re-transmission timer if it is not set. */
    void startRetransmissionTimer(SendWindow::Entry &entry);

    /** @brief Start the ACK timer if it is not set. */
    void startACKTimer(RecvWindow &recvw);

    /** @brief Handle HELLO and timestamp control messages. */
    void handleCtrlHello(Node &node, std::shared_ptr<RadioPacket>& pkt);

    /** @brief Handle timestamp delta control messages. */
    void handleCtrlTimestampDeltas(Node &node, std::shared_ptr<RadioPacket>& pkt);

    /** @brief Handle NAK control messages. */
    void handleCtrlNAK(Node &node, std::shared_ptr<RadioPacket>& pkt);

    /** @brief Append NAK control messages. */
    void appendCtrlNAK(RecvWindow &recvw, std::shared_ptr<NetPacket>& pkt);

    /** @brief Revise PER estimate based on NAK. */
    void nakUpdatePER(SendWindow &sendw, Node &dest, const Seq &seq, bool explicitNak);

    /** @brief Retransmit a NAK'ed packet. */
    void nakRetransmit(SendWindow &sendw, const Seq &seq);

    /** @brief Handle a successful packet transmission. */
    void txSuccess(SendWindow &sendw, Node &node);

    /** @brief Update PER as a result of unsuccessful packet transmission. */
    void txFailureUpdatePER(Node &node);

    /** @brief Handle an unsuccessful packet transmission based on updated PER. */
    void txFailure(SendWindow &sendw, Node &node);

    /** @brief Get a packet that is elligible to be sent. */
    bool getPacket(std::shared_ptr<NetPacket>& pkt);

    /** @brief Reconfigure a node's PER estimates */
    void resetPEREstimates(Node &node);

    /** @brief Get a node's send window.
     * @param node_id The node whose window to get
     * @returns A pointer to the window or nullptr if one doesn't exist.
     */
    SendWindow *maybeGetSendWindow(NodeId node_id);

    /** @brief Get a node's send window */
    SendWindow &getSendWindow(NodeId node_id);

    /** @brief Get a node's receive window.
     * @param node_id The node whose window to get
     * @returns A pointer to the window or nullptr if one doesn't exist.
     */
    RecvWindow *maybeGetReceiveWindow(NodeId node_id);

    /** @brief Get a node's receive window */
    RecvWindow &getReceiveWindow(NodeId node_id, Seq seq, bool isSYN);
};

#endif /* SMARTCONTROLLER_H_ */
