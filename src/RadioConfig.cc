#include "RadioConfig.hh"

RadioConfig rc;

RadioConfig::RadioConfig()
  : node_id(0)
  , verbose(false)
  , debug(false)
  , log_invalid_headers(false)
  , mtu(1500)
  , verbose_packet_trace(false)
{
}
