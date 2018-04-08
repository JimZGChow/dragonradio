#ifndef SAFEQUEUE_H_
#define SAFEQUEUE_H_

#include <condition_variable>
#include <mutex>
#include <queue>

template<typename T>
class SafeQueue {
public:
    SafeQueue() : done(false) {};

    void push(const T& val);
    void push(T&& val);
    void pop(T& val);

    void join(void);

private:
    bool                    done;
    std::mutex              m;
    std::condition_variable cond;
    std::queue<T>           q;
};

template<typename T>
void SafeQueue<T>::push(const T& val)
{
    std::lock_guard<std::mutex> lock(m);

    q.push(val);
    cond.notify_one();
}

template<typename T>
void SafeQueue<T>::push(T&& val)
{
    std::lock_guard<std::mutex> lock(m);

    q.push(std::move(val));
    cond.notify_one();
}

template<typename T>
void SafeQueue<T>::pop(T& val)
{
    std::unique_lock<std::mutex> lock(m);

    cond.wait(lock, [this]{ return done || !q.empty(); });
    if (done)
        return;
    val = std::move(q.front());
    q.pop();
}


template<typename T>
void SafeQueue<T>::join(void)
{
    done = true;
    cond.notify_all();
}

#endif /* SAFEQUEUE_H_ */