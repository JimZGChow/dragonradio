#!/usr/bin/env python3
import h5py
import numpy as np
import time

class Slot:
    def __init__(self, timestamp, iqdata):
        self._timestamp = timestamp
        self._iqdata = iqdata

    @property
    def timestamp(self):
        return self._timestamp

    @property
    def data(self):
        return self._iqdata

LIQUID_CRC = [ 'unknown'
             , 'none'
             , 'checksum'
             , 'crc8'
             , 'crc16'
             , 'crc24'
             , 'crc32' ]

LIQUID_FEC = [ 'unknown'
             , 'none'
             , 'rep3'
             , 'rep5'
             , 'h74'
             , 'h84'
             , 'h128'
             , 'g2412'
             , 'secded2216'
             , 'secded3932'
             , 'secded7264'
             , 'v27'
             , 'v29'
             , 'v39'
             , 'v615'
             , 'v27p23'
             , 'v27p34'
             , 'v27p45'
             , 'v27p56'
             , 'v27p67'
             , 'v27p78'
             , 'v29p23'
             , 'v29p34'
             , 'v29p45'
             , 'v29p56'
             , 'v29p67'
             , 'v29p78'
             , 'rs8' ]

LIQUID_MS = [ 'unknown'

              # phase-shift keying
            , 'psk2'
            , 'psk4'
            , 'psk8'
            , 'psk16'
            , 'psk32'
            , 'psk64'
            , 'psk128'
            , 'psk256'

              # differential phase-shift keying
            , 'dpsk2'
            , 'dpsk4'
            , 'dpsk8'
            , 'dpsk16'
            , 'dpsk32'
            , 'dpsk64'
            , 'dpsk128'
            , 'dpsk256'

              # amplitude-shift keying
            , 'ask2'
            , 'ask4'
            , 'ask8'
            , 'ask16'
            , 'ask32'
            , 'ask64'
            , 'ask128'
            , 'ask256'

              # quadrature amplitude-shift keying
            , 'qam4'
            , 'qam8'
            , 'qam16'
            , 'qam32'
            , 'qam64'
            , 'qam128'
            , 'qam256'

              # amplitude/phase-shift keying
            , 'apsk4'
            , 'apsk8'
            , 'apsk16'
            , 'apsk32'
            , 'apsk64'
            , 'apsk128'
            , 'apsk256'

              # specific modem types
            , 'bpsk'
            , 'qpsk'
            , 'ook'
            , 'sqam32'
            , 'sqam128'
            , 'V29'
            , 'arb16opt'
            , 'arb32opt'
            , 'arb64opt'
            , 'arb128opt'
            , 'arb256opt'
            , 'arb64vt'

              # arbitrary modem type
            , 'arb' ]

class RecvPacket:
    def __init__(self, timestamp, start, end, hdr_valid, payload_valid, pkt_id, src, dest, crc, fec0, fec1, ms, evm, rssi, iqdata):
        self._timestamp = timestamp
        self._start = start
        self._end = end
        self._hdr_valid = hdr_valid
        self._payload_valid = payload_valid
        self._pkt_id = pkt_id
        self._src = src
        self._dest = dest
        self._crc = crc
        self._fec0 = fec0
        self._fec1 = fec1
        self._ms = ms
        self._evm = evm
        self._rssi = rssi
        self._iqdata = iqdata

    def __str__(self):
        return "Packet(pkt_id={pkt_id}, src={src}, dest={dest}, ms={ms}, fec0={fec0}, fec1={fec1})".\
        format(pkt_id=self.pkt_id, src=self.src,  dest=self.dest, \
               ms=self.ms, fec0=self.fec0,  fec1=self.fec1)

    @property
    def timestamp(self):
        """Receive slot timestamp (in seconds since the logging node's start timestamp)"""
        return self._timestamp

    @property
    def start(self):
        """Packet start (in samples since the beginning of the receive slot)"""
        return self._start

    @property
    def end(self):
        """Packet end (in samples since the beginning of the receive slot)"""
        return self._end

    @property
    def hdr_valid(self):
        """Flag indicating whther or not the header is valid"""
        return self._hdr_valid

    @property
    def payload_valid(self):
        """Flag indicating whther or not the payload is valid"""
        return self._payload_valid

    @property
    def pkt_id(self):
        """Packet ID"""
        return self._pkt_id

    @property
    def src(self):
        """Source node ID"""
        return self._src

    @property
    def dest(self):
        """Destination node ID"""
        return self._dest

    @property
    def crc(self):
        """Liquid CRC"""
        return LIQUID_CRC[self._crc]

    @property
    def fec0(self):
        """Liquid FEC0"""
        return LIQUID_FEC[self._fec0]

    @property
    def fec1(self):
        """Liquid FEC1"""
        return LIQUID_FEC[self._fec1]

    @property
    def ms(self):
        """Liquid modulation scheme"""
        return LIQUID_MS[self._ms]

    @property
    def evm(self):
        """EVM (dB)"""
        return self._evm

    @property
    def rssi(self):
        """RSSI (dB)"""
        return self._rssi

    @property
    def data(self):
        """Demodulated IQ data"""
        return self._iqdata

class SendPacket:
    def __init__(self, timestamp, pkt_id, src, dest, iqdata):
        self._timestamp = timestamp
        self._pkt_id = pkt_id
        self._src = src
        self._dest = dest
        self._iqdata = iqdata

    @property
    def timestamp(self):
        """Packet timestamp (in seconds since the logging node's start timestamp)"""
        return self._timestamp

    @property
    def pkt_id(self):
        """Packet ID"""
        return self._pkt_id

    @property
    def src(self):
        """Source node ID"""
        return self._src

    @property
    def dest(self):
        """Destination node ID"""
        return self._dest

    @property
    def data(self):
        """Modulated IQ data"""
        return self._iqdata

class Node:
    def __init__(self):
        self.log_attrs = {}

    @property
    def node_id(self):
        """The node's ID"""
        return self.log_attrs['node_id']

    @property
    def start(self):
        """Time at which logging began (in seconds since the Epoch)"""
        return time.localtime(self.log_attrs['start'])

    @property
    def tx_bandwidth(self):
        """TX bandwidth (in Hz)"""
        return self.log_attrs['tx_bandwidth']

    @property
    def rx_bandwidth(self):
        """RX bandwidth (in Hz)"""
        return self.log_attrs['rx_bandwidth']

    @property
    def crc_scheme(self):
        """Liquid DSP CRC scheme"""
        return self.log_attrs['crc_scheme'].decode()

    @property
    def fec0(self):
        """Liquid DSP inner forward error correction"""
        return self.log_attrs['fec0'].decode()

    @property
    def fec1(self):
        """Liquid DSP outer forward error correction"""
        return self.log_attrs['fec1'].decode()

    @property
    def modulation_scheme(self):
        """Liquid DSP modulation scheme"""
        return self.log_attrs['modulation_scheme'].decode()

class Log:
    def __init__(self):
        self._nodes = {}
        self._logs = {}
        self._recv = {}
        self._send = {}
        self._slots = {}

    def load(self, filename):
        with h5py.File(filename, 'r') as f:
            node = Node()
            for attr in f.attrs:
                node.log_attrs[attr] = f.attrs[attr]

            self._nodes[node.node_id] = node
            self._slots[node.node_id] = [Slot(*x) for x in f['slots']]
            self._recv[node.node_id] = [RecvPacket(*x) for x in f['recv']]
            self._send[node.node_id]= [SendPacket(*x) for x in f['send']]

            self._recv[node.node_id].sort(key=lambda x: x.pkt_id)

            return node

    def findReceivedPackets(self, node, t_start, t_end):
        """
        Find all packets received by a node in a given time period.

        Args:
            node: The node.
            t_start: The start of the time period.
            t_end: The end of the time period.

        Returns:
            A list of packets.
        """
        result = []

        Fs = node.rx_bandwidth
        recv = self.received[node.node_id]

        for pkt in recv:
            t1 = pkt.timestamp + t_start/Fs
            t2 = pkt.timestamp + t_end/Fs
            if t1 >= t_start and t1 < t_end:
                result.append(pkt)

        return result

    def findSlots(self, node, pkt):
        slots = self._slots[node.node_id]

        for i in range(0, len(slots)):
            if slots[i].timestamp == pkt.timestamp:
                ts = [slots[i].timestamp, slots[i+1].timestamp]
                data = np.concatenate((slots[i].data, slots[i+1].data))
                return (ts, data)

        return None

    def findReceivedPacketIndex(self, node, pkt_id):
        """
        Find the index of a packet in a node's list of received packets.

        Args:
            node: The node.
            pkt: The packet.

        Returns:
            The index or None.
        """
        recv = self.received[node.node_id]

        for i in range(0, len(recv)):
            if recv[i].pkt_id == pkt_id:
                return i
            elif recv[i].pkt_id > pkt_id:
                return None

        return None

    def findSentPacketIndex(self, node, pkt_id):
        """
        Find the index of a packet in a node's list of sent packets.

        Args:
            node: The node.
            pkt: The packet.

        Returns:
            The index or None.
        """
        send = self.sent[node.node_id]

        for i in range(0, len(send)):
            if send[i].pkt_id == pkt_id:
                return i
            elif send[i].pkt_id > pkt_id:
                return None

        return None

    @property
    def nodes(self):
        return self._nodes

    @property
    def received(self):
        return self._recv

    @property
    def sent(self):
        return self._send