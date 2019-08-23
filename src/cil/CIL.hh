#ifndef CIL_HH_
#define CIL_HH_

#include <optional>
#include <unordered_map>

#include "net/Net.hh"

struct Mandate {
    Mandate() = delete;

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
      , achieved_duration(0)
      , scalar_performace(0.0)
    {
    }

    ~Mandate() = default;

    /** @brief Flow UID */
    const FlowUID flow_uid;

    /** @brief Period over which to measure outcome metrics (sec) */
    const double hold_period;

    /** @brief Point value */
    const int point_value;

    /** @brief Maximum latency allowed for a packet (sec) */
    const std::optional<double> max_latency_s;

    /** @brief Minimum throughput (bps) */
    const std::optional<double> min_throughput_bps;

    /** @brief File transfer delivery deadline (sec) */
    const std::optional<double> file_transfer_deadline_s;

    /** @brief Achieved duration */
    unsigned achieved_duration;

    /** @brief Scalar performance */
    double scalar_performace;

    /** @brief Nodes in flow */
    std::vector<NodeId> radio_ids;
};

using MandateMap = std::unordered_map<FlowUID, Mandate>;

#endif /* CIL_HH_ */