#include "RadioConfig.hh"

RadioConfig rc;

RadioConfig::RadioConfig()
  : verbose(false)
  , is_gateway(false)
  , short_per_npackets(50)
  , long_per_npackets(200)
{
}
