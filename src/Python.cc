#include <pybind11/embed.h>
#include <pybind11/stl_bind.h>

namespace py = pybind11;

#include "Estimator.hh"
#include "Logger.hh"
#include "RadioConfig.hh"
#include "USRP.hh"
#include "WorkQueue.hh"
#include "phy/Channels.hh"
#include "phy/FlexFrame.hh"
#include "phy/MultiOFDM.hh"
#include "phy/NewFlexFrame.hh"
#include "phy/OFDM.hh"
#include "phy/ParallelPacketModulator.hh"
#include "phy/ParallelPacketDemodulator.hh"
#include "phy/TXParams.hh"
#include "mac/Controller.hh"
#include "mac/DummyController.hh"
#include "mac/SmartController.hh"
#include "mac/SlottedALOHA.hh"
#include "mac/SlottedMAC.hh"
#include "mac/TDMA.hh"
#include "net/Element.hh"
#include "net/Net.hh"
#include "net/NetFilter.hh"
#include "net/Queue.hh"

PYBIND11_MAKE_OPAQUE(std::vector<double>)
PYBIND11_MAKE_OPAQUE(std::vector<TXParams>)

std::shared_ptr<Logger> mkLogger(const std::string& path)
{
    Clock::time_point t_start = Clock::time_point(Clock::now().get_full_secs());
    auto              log = std::make_shared<Logger>(t_start);

    log->open(path);
    log->setAttribute("start", (uint32_t) t_start.get_full_secs());

    return log;
}

void addLoggerSource(py::class_<Logger, std::shared_ptr<Logger>>& cls, const std::string &name, Logger::Source src)
{
    cls.def_property(name.c_str(),
        [src](std::shared_ptr<Logger> log) { return log->getCollectSource(src); },
        [src](std::shared_ptr<Logger> log, bool collect) { log->setCollectSource(src, collect); });
}

template <class D, class P, class T>
struct PortWrapper
{
    std::shared_ptr<Element> element;
    Port<D, P, T> *port;

    template <class U>
    PortWrapper(std::shared_ptr<U> e, Port<D, P, T> *p) :
        element(std::static_pointer_cast<Element>(e)), port(p) {}
    ~PortWrapper() = default;

    PortWrapper() = delete;
};

template <class U, class D, class P, class T>
std::unique_ptr<PortWrapper<D,P,T>> exposePort(std::shared_ptr<U> e, Port<D, P, T> *p)
{
    return std::make_unique<PortWrapper<D,P,T>>(std::static_pointer_cast<Element>(e), p);
}

template <typename D>
using NetInWrapper = PortWrapper<In,D,std::shared_ptr<NetPacket>>;

template <typename D>
using NetOutWrapper = PortWrapper<Out,D,std::shared_ptr<NetPacket>>;

using NetInPush = NetInWrapper<Push>;
using NetInPull = NetInWrapper<Pull>;
using NetOutPush = NetOutWrapper<Push>;
using NetOutPull = NetOutWrapper<Pull>;

template <typename D>
using RadioInWrapper = PortWrapper<In,D,std::shared_ptr<RadioPacket>>;

template <typename D>
using RadioOutWrapper = PortWrapper<Out,D,std::shared_ptr<RadioPacket>>;

using RadioInPush = RadioInWrapper<Push>;
using RadioInPull = RadioInWrapper<Pull>;
using RadioOutPush = RadioOutWrapper<Push>;
using RadioOutPull = RadioOutWrapper<Pull>;

// UGH. See:
//   https://stackoverflow.com/questions/240353/convert-a-preprocessor-token-to-a-string

#define TOSTRING2(s) #s
#define TOSTRING(s) TOSTRING2(s)

PYBIND11_EMBEDDED_MODULE(dragonradio, m) {
    // Export DragonRadio version
    m.attr("version") = TOSTRING(VERSION);

    // Create enum type CRCScheme for liquid CRC schemes
    py::enum_<crc_scheme> crc(m, "CRCScheme");

    crc.def(py::init([](std::string value) -> crc_scheme {
            auto crc = liquid_getopt_str2crc(value.c_str());
            if (crc == LIQUID_CRC_UNKNOWN)
               throw py::value_error("\"" + value + "\" is not a valid value for enum type CRCScheme");
            return crc;
        }));

    py::implicitly_convertible<py::str, crc_scheme>();

    for (unsigned int i = 0; i < LIQUID_CRC_NUM_SCHEMES; ++i)
        crc.value(crc_scheme_str[i][0], static_cast<crc_scheme>(i));

    crc.export_values();

    // Create enum type FECScheme for liquid FEC schemes
    py::enum_<fec_scheme> fec(m, "FECScheme");

    fec.def(py::init([](std::string value) -> fec_scheme {
            auto fec = liquid_getopt_str2fec(value.c_str());
            if (fec == LIQUID_FEC_UNKNOWN)
               throw py::value_error("\"" + value + "\" is not a valid value for enum type FECScheme");
            return fec;
        }));

    py::implicitly_convertible<py::str, fec_scheme>();

    for (unsigned int i = 0; i < LIQUID_FEC_NUM_SCHEMES; ++i)
        fec.value(fec_scheme_str[i][0], static_cast<fec_scheme>(i));

    fec.export_values();

    // Create enum type ModulationScheme for liquid modulation schemes
    py::enum_<modulation_scheme> ms(m, "ModulationScheme");

    ms.def(py::init([](std::string value) -> modulation_scheme {
           auto ms = liquid_getopt_str2mod(value.c_str());
           if (ms == LIQUID_MODEM_UNKNOWN)
               throw py::value_error("\"" + value + "\" is not a valid value for enum type ModulationScheme");
           return ms;
       }));

    py::implicitly_convertible<py::str, modulation_scheme>();

    for (unsigned int i = 0; i < LIQUID_MODEM_NUM_SCHEMES; ++i)
        ms.value(modulation_types[i].name, static_cast<modulation_scheme>(i));

    ms.export_values();

    // Export class Logger to Python
    py::class_<Logger, std::shared_ptr<Logger>> loggerCls(m, "Logger");

    loggerCls
        .def_property_static("singleton",
            [](py::object) { return logger; },
            [](py::object, std::shared_ptr<Logger> log) { return logger = log; })
        .def(py::init(&mkLogger))
        .def("setAttribute", py::overload_cast<const std::string&, const std::string&>(&Logger::setAttribute))
        .def("setAttribute", py::overload_cast<const std::string&, uint8_t>(&Logger::setAttribute))
        .def("setAttribute", py::overload_cast<const std::string&, uint32_t>(&Logger::setAttribute))
        .def("setAttribute", py::overload_cast<const std::string&, double>(&Logger::setAttribute))
        ;

    addLoggerSource(loggerCls, "log_slots", Logger::kSlots);
    addLoggerSource(loggerCls, "log_recv_packets", Logger::kRecvPackets);
    addLoggerSource(loggerCls, "log_recv_data", Logger::kRecvData);
    addLoggerSource(loggerCls, "log_sent_packets", Logger::kSentPackets);
    addLoggerSource(loggerCls, "log_sent_data", Logger::kSentData);
    addLoggerSource(loggerCls, "log_events", Logger::kEvents);

    // Export class RadioConfig to Python
    py::class_<RadioConfig, std::shared_ptr<RadioConfig>>(m, "RadioConfig")
        .def(py::init())
        .def_readwrite("verbose", &RadioConfig::verbose,
            "Output verbose messages to the console")
        .def_readwrite("debug", &RadioConfig::debug,
            "Output debug messages to the console")
        .def_readwrite("amc_short_per_nslots", &RadioConfig::amc_short_per_nslots,
            "Number of slots worth of packets we use to calculate short-term PER")
        .def_readwrite("amc_long_per_nslots", &RadioConfig::amc_long_per_nslots,
            "Number of lots worth of packets we use to calculate long-term PER")
        .def_readwrite("timestamp_delay", &RadioConfig::timestamp_delay,
            "Timestamp delay, in seconds")
        .def_readwrite("max_packet_size", &RadioConfig::max_packet_size,
            "Maximum size of a packet, in bytes")
        .def_readwrite("arq_ack_delay", &RadioConfig::arq_ack_delay,
            "ACK delay, in seconds")
        .def_readwrite("arq_retransmission_delay", &RadioConfig::arq_retransmission_delay,
            "Retransmission delay, in seconds")
        .def_readwrite("slot_modulate_time", &RadioConfig::slot_modulate_time,
            "Time needed to modulate a slot's worth of data, in seconds")
        .def_readwrite("slot_send_time", &RadioConfig::slot_send_time,
            "Time needed to send a slot's worth of data, in seconds")
        ;

    // Export our global RadioConfig
    m.attr("rc") = py::cast(rc, py::return_value_policy::reference);

    // Export class WorkQueue to Python
    py::class_<WorkQueue, std::shared_ptr<WorkQueue>>(m, "WorkQueue")
        .def("addThreads", &WorkQueue::addThreads,
            "Add workers")
        ;

    // Export our global WorkQueue
    m.attr("work_queue") = py::cast(work_queue, py::return_value_policy::reference);

    // Export class USRP to Python
    py::enum_<USRP::DeviceType>(m, "DeviceType")
        .value("N210", USRP::kUSRPN210)
        .value("X310", USRP::kUSRPX310)
        .value("Unknown", USRP::kUSRPUnknown)
        .export_values();

    py::class_<USRP, std::shared_ptr<USRP>>(m, "USRP")
        .def(py::init<const std::string&,
                      double,
                      const std::string&,
                      const std::string&,
                      float,
                      float>())
        .def_property_readonly("device_type", &USRP::getDeviceType)
        .def_property("tx_frequency", &USRP::getTXFrequency, &USRP::setTXFrequency)
        .def_property("rx_frequency", &USRP::getRXFrequency, &USRP::setRXFrequency)
        .def_property("tx_rate", &USRP::getTXRate, &USRP::setTXRate)
        .def_property("rx_rate", &USRP::getRXRate, &USRP::setRXRate)
        .def_property("tx_gain", &USRP::getTXGain, &USRP::setTXGain)
        .def_property("rx_gain", &USRP::getRXGain, &USRP::setRXGain)
        .def_property("tx_max_samps", &USRP::getMaxTXSamps, &USRP::setMaxTXSamps)
        .def_property("rx_max_samps", &USRP::getMaxRXSamps, &USRP::setMaxRXSamps)
        ;

    // Export port wrapper classes to Python
    py::class_<NetInPush, std::unique_ptr<NetInPush>>(m, "NetInPush")
        .def("__lshift__", [](NetInPush *in, NetOutPush *out) { out->port->connect(in->element, in->port); } )
        .def("disconnect", [](NetInPush *in) { if (in->port->isConnected()) { reinterpret_cast<NetOut<Push>*>(in->port->partner())->disconnect(); } })
        ;
    py::class_<NetInPull, std::unique_ptr<NetInPull>>(m, "NetInPull")
        .def("__lshift__", [](NetInPull *in, NetOutPull *out) { in->port->connect(out->element, out->port); } )
        .def("disconnect", [](NetInPull *in) { in->port->disconnect(); } )
        ;
    py::class_<NetOutPull, std::unique_ptr<NetOutPull>>(m, "NetOutPull")
        .def("__rshift__", [](NetOutPull *out, NetInPull *in) { in->port->connect(out->element, out->port); } )
        .def("disconnect", [](NetOutPull *out) { if (out->port->isConnected()) { reinterpret_cast<NetIn<Pull>*>(out->port->partner())->disconnect(); } })
        ;
    py::class_<NetOutPush, std::unique_ptr<NetOutPush>>(m, "NetOutPush")
        .def("__rshift__", [](NetOutPush *out, NetInPush *in) { out->port->connect(in->element, in->port); } )
        .def("disconnect", [](NetOutPush *out) { out->port->disconnect(); } )
        ;

    py::class_<RadioInPush, std::unique_ptr<RadioInPush>>(m, "RadioInPush")
        .def("__lshift__", [](RadioInPush *in, RadioOutPush *out) { out->port->connect(in->element, in->port); } )
        .def("disconnect", [](RadioInPush *in) { if (in->port->isConnected()) { reinterpret_cast<RadioOut<Push>*>(in->port->partner())->disconnect(); } })
        ;
    py::class_<RadioInPull, std::unique_ptr<RadioInPull>>(m, "RadioInPull")
        .def("__lshift__", [](RadioInPull *in, RadioOutPull *out) { in->port->connect(out->element, out->port); } )
        .def("disconnect", [](RadioInPull *in) { in->port->disconnect(); } )
        ;
    py::class_<RadioOutPull, std::unique_ptr<RadioOutPull>>(m, "RadioOutPull")
        .def("__rshift__", [](RadioOutPull *out, RadioInPull *in) { in->port->connect(out->element, out->port); } )
        .def("disconnect", [](RadioOutPull *out) { if (out->port->isConnected()) { reinterpret_cast<RadioIn<Pull>*>(out->port->partner())->disconnect(); } })
        ;
    py::class_<RadioOutPush, std::unique_ptr<RadioOutPush>>(m, "RadioOutPush")
        .def("__rshift__", [](RadioOutPush *out, RadioInPush *in) { out->port->connect(in->element, in->port); } )
        .def("disconnect", [](RadioOutPush *out) { out->port->disconnect(); } )
        ;

    // Export class NetQueue to Python
    py::class_<NetQueue, std::shared_ptr<NetQueue>>(m, "NetQueue")
        .def(py::init())
        .def_property_readonly("push", [](std::shared_ptr<NetQueue> element) { return exposePort(element, &element->in); } )
        .def_property_readonly("pop", [](std::shared_ptr<NetQueue> element) { return exposePort(element, &element->out); } )
        ;

    // Export class NetFilter to Python
    py::class_<NetFilter, std::shared_ptr<NetFilter>>(m, "NetFilter")
        .def(py::init<std::shared_ptr<Net>>())
        .def_property_readonly("input", [](std::shared_ptr<NetFilter> element) { return exposePort(element, &element->in); } )
        .def_property_readonly("output", [](std::shared_ptr<NetFilter> element) { return exposePort(element, &element->out); } )
        ;

    // Export class TunTap to Python
    py::class_<TunTap, std::shared_ptr<TunTap>>(m, "TunTap")
        .def(py::init<const std::string&,
                      bool,
                      size_t,
                      uint8_t>())
        .def_property_readonly("mtu", &TunTap::getMTU)
        .def_property_readonly("source", [](std::shared_ptr<TunTap> element) { return exposePort(element, &element->source); } )
        .def_property_readonly("sink", [](std::shared_ptr<TunTap> element) { return exposePort(element, &element->sink); } )
        ;

    // Export estimator classes to Python
    py::class_<Estimator<float>, std::shared_ptr<Estimator<float>>>(m, "Estimator")
        .def_property_readonly("value", &Estimator<float>::getValue, "The value of the estimator")
        .def_property_readonly("nsamples", &Estimator<float>::getNSamples, "The number of samples used in the estimate")
        .def("reset", &Estimator<float>::reset, "Reset the estimate")
        .def("update", &Estimator<float>::update, "Update the estimate")
        ;

    py::class_<Mean<float>, Estimator<float>, std::shared_ptr<Mean<float>>>(m, "Mean")
        .def(py::init<>())
        .def(py::init<float>())
        .def("remove", &Mean<float>::remove, "Remove a value used to estimate the mean")
        ;

    // Export class MCS to Python
    py::class_<MCS, std::shared_ptr<MCS>>(m, "MCS")
        .def(py::init<>())
        .def(py::init<crc_scheme,
                      fec_scheme,
                      fec_scheme,
                      modulation_scheme>())
        .def_readwrite("check", &MCS::check, "Data validity check")
        .def_readwrite("fec0", &MCS::fec0, "Inner FEC")
        .def_readwrite("fec1", &MCS::fec1, "Outer FEC")
        .def_readwrite("ms", &MCS::ms, "Modulation scheme")
        .def_property_readonly("rate", &MCS::getRate, "Approximate rate (bps)")
        ;

    // Export class TXParams to Python
    py::class_<TXParams, std::shared_ptr<TXParams>>(m, "TXParams")
        .def(py::init<>())
        .def(py::init<MCS>())
        .def_readwrite("mcs", &TXParams::mcs, "Modulation and coding scheme")
        .def_readwrite("g_0dBFS", &TXParams::g_0dBFS, "Soft TX gain (multiplicative factor)")
        .def_property("soft_tx_gain_0dBFS", &TXParams::getSoftTXGain0dBFS, &TXParams::setSoftTXGain0dBFS, "Soft TX gain (dBFS)")
        .def_readwrite("auto_soft_tx_gain_clip_frac", &TXParams::auto_soft_tx_gain_clip_frac, "Clipping threshold for automatic TX soft gain")
        .def("recalc0dBFSEstimate", &TXParams::recalc0dBFSEstimate, "Reset the 0dBFS estimate")
        ;

    py::bind_vector<std::vector<TXParams>>(m, "TXParamsVector");

    // Export class Node to Python
    py::class_<Node, std::shared_ptr<Node>>(m, "Node")
        .def_readonly("id", &Node::id, "Node ID")
        .def_readwrite("is_gateway", &Node::is_gateway, "Flag indicating whether or not this node is the gateway")
        // Make a copy of the Node's TXParams because it shouldn't be modified
        // directly since it is NOT owned by the node.
        .def_property_readonly("tx_params", [](Node &node) { return *node.tx_params; }, "TX parameters",
            py::return_value_policy::copy)
        .def_readwrite("g", &Node::g, "Soft TX gain (multiplicative)")
        .def_property("soft_tx_gain", &Node::getSoftTXGain, &Node::setSoftTXGain, "Soft TX gain (dBFS)")
        .def_readwrite("ack_delay", &Node::ack_delay, "ACK delay (in seconds)")
        .def_readwrite("retransmission_delay", &Node::retransmission_delay, "Packet retransmission delay (in seconds)")
        .def_property_readonly("short_per", [](Node &node) { return node.short_per.getValue(); }, "Short-term packet error rate (unitless)")
        .def_property_readonly("long_per", [](Node &node) { return node.long_per.getValue(); }, "Long-term packet error rate (unitless)")
        ;

    // Export class Net to Python
    py::class_<Net, std::shared_ptr<Net>>(m, "Net")
        .def(py::init<std::shared_ptr<TunTap>,
                      NodeId>())
        .def("__getitem__", [](Net &net, NodeId key) -> Node& {
            try {
                return net[key];
            } catch (const std::out_of_range&) {
                throw py::key_error("key '" + std::to_string(key) + "' does not exist");
            }
        }, py::return_value_policy::reference_internal)
        .def("__len__", &Net::size)
        .def("__iter__", [](Net &net) {
            return py::make_key_iterator(net.begin(), net.end());
         }, py::keep_alive<0, 1>())
        .def("keys", [](Net &net) {
            return py::make_key_iterator(net.begin(), net.end());
         }, py::keep_alive<0, 1>())
        .def("items", [](Net &net) {
            return py::make_iterator(net.begin(), net.end());
         }, py::keep_alive<0, 1>(), py::return_value_policy::reference_internal)
        .def_property_readonly("my_node_id", &Net::getMyNodeId)
        .def_readwrite("tx_params", &Net::tx_params, "TX parameters")
        .def("addNode", &Net::addNode, py::return_value_policy::reference_internal)
        ;

    // Export class PHY to Python
    py::class_<PHY, std::shared_ptr<PHY>>(m, "PHY")
        .def_property_readonly("min_rx_rate_oversample", &PHY::getMinRXRateOversample)
        .def_property_readonly("min_tx_rate_oversample", &PHY::getMinTXRateOversample)
        .def_property("rx_rate", &PHY::getRXRate, &PHY::setRXRate)
        .def_property("tx_rate", &PHY::getTXRate, &PHY::setTXRate)
        .def_property("rx_rate_oversample", &PHY::getRXRateOversample, &PHY::setRXRateOversample)
        .def_property("tx_rate_oversample", &PHY::getTXRateOversample, &PHY::setTXRateOversample)
        ;

    // Export class LiquidPHY to Python
    py::class_<LiquidPHY, PHY, std::shared_ptr<LiquidPHY>>(m, "LiquidPHY")
        .def_property_readonly("header_mcs", &LiquidPHY::getHeaderMCS)
        .def_property_readonly("soft_header", &LiquidPHY::getSoftHeader)
        .def_property_readonly("soft_payload", &LiquidPHY::getSoftPayload)
        .def_readonly("min_packet_size", &LiquidPHY::min_packet_size)
        ;

    // Export class FlexFrame to Python
    py::class_<FlexFrame, LiquidPHY, std::shared_ptr<FlexFrame>>(m, "FlexFrame")
        .def(py::init<const MCS&,
                      bool,
                      bool,
                      size_t>())
        ;

    // Export class NewFlexFrame to Python
    py::class_<NewFlexFrame, LiquidPHY, std::shared_ptr<NewFlexFrame>>(m, "NewFlexFrame")
        .def(py::init<const MCS&,
                      bool,
                      bool,
                      size_t>())
        ;

    // Export class OFDM to Python
    py::class_<OFDM, LiquidPHY, std::shared_ptr<OFDM>>(m, "OFDM")
        .def(py::init<const MCS&,
                      bool,
                      bool,
                      size_t,
                      unsigned int,
                      unsigned int,
                      unsigned int>())
        ;

    // Export class MultiOFDM to Python
    py::class_<MultiOFDM, LiquidPHY, std::shared_ptr<MultiOFDM>>(m, "MultiOFDM")
        .def(py::init<size_t,
                      unsigned int,
                      unsigned int,
                      unsigned int>())
        ;

    // Export class PacketModulator to Python
    py::class_<PacketModulator, std::shared_ptr<PacketModulator>>(m, "PacketModulator")
        .def_property("tx_channel", &PacketModulator::getTXChannel, &PacketModulator::setTXChannel)
        ;

    // Export class ParallelPacketModulator to Python
    py::class_<ParallelPacketModulator, PacketModulator, std::shared_ptr<ParallelPacketModulator>>(m, "ParallelPacketModulator")
        .def(py::init<std::shared_ptr<Net>,
                      std::shared_ptr<PHY>,
                      std::shared_ptr<Channels>,
                      unsigned int>())
        .def_property_readonly("sink", [](std::shared_ptr<ParallelPacketModulator> element) { return exposePort(element, &element->sink); } )
        ;

    // Export class PacketDemodulator to Python
    py::class_<PacketDemodulator, std::shared_ptr<PacketDemodulator>>(m, "PacketDemodulator")
        .def("setWindowParameters", &PacketDemodulator::setWindowParameters)
        ;

    // Export class ParallelPacketDemodulator to Python
    py::class_<ParallelPacketDemodulator, PacketDemodulator, std::shared_ptr<ParallelPacketDemodulator>>(m, "ParallelPacketDemodulator")
        .def(py::init<std::shared_ptr<Net>,
                      std::shared_ptr<PHY>,
                      std::shared_ptr<Channels>,
                      unsigned int>())
        .def_property("enforce_ordering", &ParallelPacketDemodulator::getEnforceOrdering, &ParallelPacketDemodulator::setEnforceOrdering)
        .def_property_readonly("source", [](std::shared_ptr<ParallelPacketDemodulator> e) { return exposePort(e, &e->source); } )
        ;

    // Export class Controller to Python
    py::class_<Controller, std::shared_ptr<Controller>>(m, "Controller")
        .def_property_readonly("net_in", [](std::shared_ptr<Controller> element) { return exposePort(element, &element->net_in); } )
        .def_property_readonly("net_out", [](std::shared_ptr<Controller> element) { return exposePort(element, &element->net_out); } )
        .def_property_readonly("radio_in", [](std::shared_ptr<Controller> element) { return exposePort(element, &element->radio_in); } )
        .def_property_readonly("radio_out", [](std::shared_ptr<Controller> element) { return exposePort(element, &element->radio_out); } )
        ;

    // Export class DummyController to Python
    py::class_<DummyController, Controller, std::shared_ptr<DummyController>>(m, "DummyController")
        .def(py::init<std::shared_ptr<Net>>())
        ;

    // Export class SmartController to Python
    py::class_<SmartController, Controller, std::shared_ptr<SmartController>>(m, "SmartController")
        .def(py::init<std::shared_ptr<Net>,
                      Seq::uint_type,
                      Seq::uint_type,
                      double,
                      double>())
        .def_property("net_queue", &SmartController::getNetQueue, &SmartController::setNetQueue)
        .def_property("mac", &SmartController::getMAC, &SmartController::setMAC)
        .def_readwrite("broadcast_tx_params", &SmartController::broadcast_tx_params, "Broadcast TX parameters",
             py::return_value_policy::reference_internal)
        .def_property("slot_size",
            &SmartController::getSlotSize,
            &SmartController::setSlotSize,
            "Number of samples in a transmission slot")
        .def_property("modidx_up_per_threshold",
            &SmartController::getUpPERThreshold,
            &SmartController::setUpPERThreshold,
            "PER threshold for increasing modulation level")
        .def_property("modidx_down_per_threshold",
            &SmartController::getDownPERThreshold,
            &SmartController::setDownPERThreshold,
            "PER threshold for decreasing modulation level")
        .def_property("enforce_ordering", &SmartController::getEnforceOrdering, &SmartController::setEnforceOrdering)
        .def("broadcastHello", &SmartController::broadcastHello)
        ;

    // Export class Channels to Python
    py::bind_vector<std::vector<double>, std::shared_ptr<std::vector<double>>>(m, "Channels");

    // Export class MAC to Python
    py::class_<MAC, std::shared_ptr<MAC>>(m, "MAC")
        .def_property("tx_channel", &MAC::getTXChannel, &MAC::setTXChannel)
        ;

    // Export class SlottedMAC to Python
    py::class_<SlottedMAC, MAC, std::shared_ptr<SlottedMAC>>(m, "SlottedMAC")
        .def_property("slot_size", &SlottedMAC::getSlotSize, &SlottedMAC::setSlotSize)
        .def_property("guard_size", &SlottedMAC::getGuardSize, &SlottedMAC::setGuardSize)
        .def_property("demod_overlap_size", &SlottedMAC::getDemodOverlapSize, &SlottedMAC::setDemodOverlapSize)
        ;

    // Export class TDMA::Slots to Python
    py::class_<TDMA::Slots, std::shared_ptr<TDMA::Slots>>(m, "Slots")
        .def("__getitem__", [](TDMA::Slots &slots, TDMA::Slots::slots_type::size_type i) {
            try {
                return slots[i];
            } catch (const std::out_of_range&) {
                throw py::index_error();
            }
        })
        .def("__setitem__", [](TDMA::Slots &slots, TDMA::Slots::slots_type::size_type i, bool v) {
            try {
                slots[i] = v;
            } catch (const std::out_of_range&) {
              throw py::index_error();
            }
        })
        .def("__len__", &TDMA::Slots::size)
        .def("__iter__", [](TDMA::Slots &slots) {
            return py::make_iterator(slots.begin(), slots.end());
         }, py::keep_alive<0, 1>())
        .def("resize", &TDMA::Slots::resize)
        ;

    // Export class TDMA to Python
    py::class_<TDMA, SlottedMAC, std::shared_ptr<TDMA>>(m, "TDMA")
        .def(py::init<std::shared_ptr<USRP>,
                      std::shared_ptr<PHY>,
                      std::shared_ptr<Channels>,
                      std::shared_ptr<PacketModulator>,
                      std::shared_ptr<PacketDemodulator>,
                      double,
                      double,
                      double,
                      size_t>())
        .def_property_readonly("slots", &TDMA::getSlots,
            py::return_value_policy::reference_internal);
        ;

    // Export class SlottedALOHA to Python
    py::class_<SlottedALOHA, SlottedMAC, std::shared_ptr<SlottedALOHA>>(m, "SlottedALOHA")
        .def(py::init<std::shared_ptr<USRP>,
                      std::shared_ptr<PHY>,
                      std::shared_ptr<Channels>,
                      std::shared_ptr<PacketModulator>,
                      std::shared_ptr<PacketDemodulator>,
                      double,
                      double,
                      double,
                      double>())
        .def_property("p", &SlottedALOHA::getTXProb, &SlottedALOHA::setTXProb)
        ;
}
