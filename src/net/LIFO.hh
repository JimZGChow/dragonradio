#ifndef LIFO_HH_
#define LIFO_HH_

#include "net/Queue.hh"

/** @brief A LIFO queue Element. */
template <class T>
class LIFO : public SimpleQueue<T> {
public:
    using SimpleQueue<T>::canPop;
    using SimpleQueue<T>::done_;
    using SimpleQueue<T>::m_;
    using SimpleQueue<T>::cond_;
    using SimpleQueue<T>::hiq_;
    using SimpleQueue<T>::q_;

    LIFO() = default;

    virtual ~LIFO() = default;

    virtual bool pop(T& val) override
    {
        std::unique_lock<std::mutex> lock(m_);

        cond_.wait(lock, [this]{ return done_ || !hiq_.empty() || !q_.empty(); });

        // If we're done, we're done
        if (done_)
            return false;

        MonoClock::time_point now = MonoClock::now();

        // First look in high-priority queue
        if (!hiq_.empty()) {
            val = std::move(hiq_.front());
            hiq_.pop_front();
            return true;
        }

        // Then look in the network queue, LIFO-style
        {
            auto it = q_.rbegin();

            while (it != q_.rend()) {
                if ((*it)->shouldDrop(now)) {
                    it = decltype(it){ q_.erase(std::next(it).base()) };
                } else if (canPop(*it)) {
                    val = std::move(*it);
                    q_.erase(std::next(it).base());
                    return true;
                } else
                    it++;
            }
        }

        return false;
    }
};

using NetLIFO = LIFO<std::shared_ptr<NetPacket>>;

using RadioLIFO = LIFO<std::shared_ptr<RadioPacket>>;

#endif /* LIFO_HH_ */