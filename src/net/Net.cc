#include <sys/types.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#include <cstddef>
#include <cstring>
#include <functional>

#include "RadioConfig.hh"
#include "Util.hh"
#include "net/Net.hh"

using namespace std::placeholders;

Node::Node(NodeId id, TXParams *tx_params)
  : id(id)
  , is_gateway(false)
  , seq(0)
  , tx_params(tx_params)
  , g(1.0)
  , ack_delay(100e-3)
  , retransmission_delay(500e-3)
  // We want the last 10 entries to account for 86% of the EMA
  , per(2.0/11.0)
{
}

Node::~Node()
{
}

Net::Net(std::shared_ptr<TunTap> tuntap,
         NodeId nodeId)
  : tuntap_(tuntap)
  , my_node_id_(nodeId)
{
}

Net::~Net()
{
}

NodeId Net::getMyNodeId(void)
{
    return my_node_id_;
}

Net::map_type::size_type Net::size(void)
{
    std::lock_guard<std::mutex> lock(nodes_mutex_);

    return nodes_.size();
}

bool Net::contains(NodeId nodeid)
{
    std::lock_guard<std::mutex> lock(nodes_mutex_);

    return nodes_.count(nodeid) == 1;
}

Net::map_type::iterator Net::begin(void)
{
    return nodes_.begin();
}

Net::map_type::iterator Net::end(void)
{
    return nodes_.end();
}

Node& Net::operator[](NodeId nodeid)
{
    std::lock_guard<std::mutex> lock(nodes_mutex_);

    return nodes_.at(nodeid);
}

Node &Net::addNode(NodeId nodeId)
{
    std::lock_guard<std::mutex> lock(nodes_mutex_);

    auto entry = nodes_.emplace(nodeId, Node(nodeId, &default_tx_params));

    // If the entry is new, add an ARP entry for it
    if (entry.second && nodeId != my_node_id_)
        tuntap_->addARPEntry(nodeId);

    return entry.first->second;
}
