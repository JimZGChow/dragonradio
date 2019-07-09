#ifndef CIL_HH_
#define CIL_HH_

#include <optional>
#include <unordered_map>

#include "net/Net.hh"

struct Mandate {
    Mandate()
      : hold_period(1.0)
    {
    }

    Mandate(FlowUID flow_uid_,
            double hold_period_,
            int point_value_,
            std::optional<double> max_latency_s_,
            std::optional<double> min_throughput_bps_,
            std::optional<double> file_transfer_deadline_s_)
      : flow_uid(flow_uid_)
      , hold_period(hold_period_)
      , point_value(point_value_)
      , max_latency_s(max_latency_s_)
      , min_throughput_bps(min_throughput_bps_)
      , file_transfer_deadline_s(file_transfer_deadline_s_)
    {
    }

    ~Mandate() = default;

    /** @brief Flow UID */
    FlowUID flow_uid;

    /** @brief Period over which to measure outcome metrics (sec) */
    double hold_period;

    /** @brief Point value */
    int point_value;

    /** @brief Maximum latency allowed for a packet (sec) */
    std::optional<double> max_latency_s;

    /** @brief Minimum throughput (bps) */
    std::optional<double> min_throughput_bps;

    /** @brief File transfer delivery deadline (sec) */
    std::optional<double> file_transfer_deadline_s;
};

using MandateMap = std::unordered_map<FlowUID, Mandate>;

#endif /* CIL_HH_ */
