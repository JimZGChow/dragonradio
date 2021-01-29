// Copyright 2018-2020 Drexel University
// Author: Geoffrey Mainland <mainland@drexel.edu>

#ifndef UTIL_HH_
#define UTIL_HH_

#include <signal.h>

#include <thread>

/** @brief sprintf to a std::string. */
std::string sprintf(const char *fmt, ...)
#if !defined(DOXYGEN)
__attribute__((format(printf, 1, 2)))
#endif
;

/** @brief Exec a command. */
int exec(const std::vector<std::string>&);

/** @brief Make thread have real-time priority. */
void setRealtimePriority(void);

/** @brief Make current thread high-priority. */
void makeThisThreadHighPriority(void);

/** @brief Pin thread to given CPU */
void pinThreadToCPU(pthread_t t, int cpu_num);

/** @brief Pin this thread to a CPU */
void pinThisThread(void);

/** @brief Sleep for the specified number of seconds. sleep, usleep, and
 * nanosleep were already taken, so this function is named "doze."
 * @param sec The number of seconds to sleep.
 * @returns -1 if interrupted.
 */
int doze(double sec);

/** @brief The signal we use to wake a thread */
const int SIGWAKE = SIGUSR1;

/** @brief Atomically block a signal */
class BlockSignal {
public:
    /** @brief Save current signal mask and block a signal
     * @param sig The signal to block
     */
    explicit BlockSignal(int sig);

    BlockSignal() = delete;

    /** @brief Restore original signal mask */
    ~BlockSignal();

    /** @brief Atomically unblock signal and pause until we receive a signal */
    void unblockAndPause(void);

protected:
    /** @brief Original signal mask before we blocked a signal */
    sigset_t orig_mask_;
};

/** @brief Make the current thread wakeable. */
/** This function installs a signal handler for SIGUSR1 so that the thread can
 * be woken from a blocking sycall via wakeThread.
 */
void makeThreadWakeable(void);

/** @brief Wake the given thread. */
/** This function sends a SIGUSR1 signal to the given thread to wake it from any
 * blocking syscalls.
 */
void wakeThread(std::thread& t);

#endif /* UTIL_HH_ */
