from concurrent.futures import CancelledError
import functools
import struct
import time

from dragon.protobuf import *
from dragon.internal_pb2 import *
import dragon.internal_pb2 as internal

INTERNAL_PORT = 8889

#
# Monkey patch Timestamp class to support setting timestamps using
# floating-point seconds.
#
def set_timestamp(self, ts):
    self.seconds = int(ts)
    self.picoseconds = int(ts % 1 * 1e12)

def get_timestamp(self):
    return self.seconds + self.picoseconds*1e-12

internal.TimeStamp.set_timestamp = set_timestamp
internal.TimeStamp.get_timestamp = get_timestamp

@handler(internal.Message)
class InternalAgent(UDPProtoServer, UDPProtoClient):
    def __init__(self,
                 controller,
                 loop=None,
                 local_ip=None,
                 server_host=None):
        UDPProtoServer.__init__(self, loop=loop)
        UDPProtoClient.__init__(self, loop=loop, server_host=server_host, server_port=INTERNAL_PORT)

        self.controller = controller

        self.loop = loop

        self.startServer(internal.Message, local_ip, INTERNAL_PORT)

    def startClient(self, server_host):
        self.server_host = server_host
        self.open()

    async def status_update(self):
        config = self.controller.config

        try:
            while True:
                me = self.controller.thisNode()

                if self.server_host:
                    await self.sendStatus()

                await asyncio.sleep(config.status_update_period)
        except CancelledError:
            pass

    @handle('Message.status')
    def handle_status(self, msg):
        node_id = msg.status.radio_id

        # Update node location
        if node_id in self.controller.nodes:
            n = self.controller.nodes[node_id]
            loc = msg.status.loc

            n.loc.lat = loc.location.latitude
            n.loc.lon = loc.location.longitude
            n.loc.alt = loc.location.elevation
            n.loc.timestamp = loc.timestamp.get_timestamp()

        # Update set of active flows
        for flow in msg.status.source_flows:
            self.controller.addLink(node_id, flow.dest, flow.flow_uid)

        # Update flow statistics
        for flow in msg.status.sink_flows:
            self.controller.addLink(flow.src, node_id, flow.flow_uid)
            self.controller.updateMandateStats(flow.flow_uid,
                                               flow.latency,
                                               flow.throughput,
                                               flow.bytes)

    @send(internal.Message)
    async def sendStatus(self, msg):
        me = self.controller.thisNode()

        radio = self.controller.radio

        msg.status.radio_id = me.id
        msg.status.timestamp.set_timestamp(time.time())
        msg.status.loc.location.latitude = me.loc.lat
        msg.status.loc.location.longitude = me.loc.lon
        msg.status.loc.location.elevation = me.loc.alt
        msg.status.loc.timestamp.set_timestamp(me.loc.timestamp)
        msg.status.source_flows.extend(copyFlowInfo(radio.flowsource.flows))
        msg.status.sink_flows.extend(copyFlowInfo(radio.flowsink.flows))

def copyFlowInfo(flows):
    internal_flows = []

    for flow_uid, flow_info in flows.items():
        info = internal.FlowInfo()
        info.flow_uid = flow_uid
        info.src = flow_info.src
        info.dest = flow_info.dest
        info.window = flow_info.latency.time_window
        info.latency = flow_info.latency.value
        info.throughput = flow_info.throughput.value
        info.bytes = flow_info.bytes

        internal_flows.append(info)

    return internal_flows
