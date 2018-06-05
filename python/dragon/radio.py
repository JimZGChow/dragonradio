import configparser
import io
import libconf
import logging
import os
import platform
import re
import sys

import dragonradio

logger = logging.getLogger('radio')

def fail(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)

def getNodeIdFromHostname():
    m = re.search(r'([0-9]{1,3})$', platform.node())
    if m:
        return int(m.group(1))
    else:
        logger.warning('Cannot determine node id from hostname')
        return None

class Config(object):
    def __init__(self):
        # Set some default values
        self.node_id = getNodeIdFromHostname()
        self.logdir_ = None
        # This is the default frequency in the Colosseum
        self.frequency = 1e9
        self.collab_server_port = 5556
        self.collab_client_port = 5557
        self.collab_peer_port = 5558

    def get_ms(self):
        return self.ms_

    def set_ms(self, ms):
        self.ms_ = dragonradio.ModulationScheme(ms)

    ms = property(get_ms, set_ms)

    def get_check(self):
        return self.check_

    def set_check(self, check):
        self.check_ = dragonradio.CRCScheme(check)

    check = property(get_check, set_check)

    def get_fec0(self):
        return self.fec0_

    def set_fec0(self, fec0):
        self.fec0_ = dragonradio.FECScheme(fec0)

    fec0 = property(get_fec0, set_fec0)

    def get_fec1(self):
        return self.fec1_

    def set_fec1(self, fec1):
        self.fec1_ = dragonradio.FECScheme(fec1)

    fec1 = property(get_fec1, set_fec1)

    @property
    def logdir(self):
        if self.logdir_:
            return self.logdir_

        if not hasattr(self, 'log_directory'):
            return None

        logdir = os.path.join(self.log_directory, 'node-{:03d}'.format(self.node_id))
        if not os.path.exists(logdir):
            os.makedirs(logdir)

        self.logdir_ = os.path.abspath(logdir)
        return self.logdir_

    def mergeConfig(self, dict):
        for key in dict:
            setattr(self, key, dict[key])

    def loadConfig(self, path):
        """
        Load configuration parameters from a radio.conf file in libconf format.
        """
        try:
            with io.open(path) as f:
                self.mergeConfig(libconf.load(f))
        except:
            logger.exception("Cannot load radio config '%s'", path)

    def loadArgs(self, args):
        """
        Load configuration parameters from command-line arguments.
        """
        # Forget arguments that aren't set so that they don't override existing
        # settings
        dict = args.__dict__
        keys = list(dict.keys())

        for key in keys:
            if dict[key] == None:
                del dict[key]

        self.mergeConfig(dict)

    def loadColosseumIni(self, path):
        """
        Load configuration parameters from a colosseum_config.ini file.
        """
        try:
            config = configparser.ConfigParser()
            config.read(path)

            if 'COLLABORATION' in config:
                for key in config['COLLABORATION']:
                    setattr(self, key, config['COLLABORATION'][key])

            if 'RF' in config:
                for key in config['RF']:
                    setattr(self, key, float(config['COLLABORATION'][key]))
        except:
            logger.exception('Cannot load colosseum_config.ini')

    def addArguments(self, parser, allow_defaults=True):
        """
        Populate an ArgumentParser with arguments corresponding to configuration
        parameters.
        """
        def enumHelp(cls):
            return ', '.join(sorted(cls.__members__.keys()))

        def add_argument(*args, default=None, **kwargs):
            if allow_defaults:
                parser.add_argument(*args, default=default, **kwargs)
            else:
                parser.add_argument(*args, **kwargs)

        add_argument('-l', action='store',
                     default=None,
                     dest='log_directory',
                     help='specify directory for log files')
        add_argument('--addr', action='store',
                     default='',
                     dest='addr',
                     help='specify device address')
        add_argument('--rx-antenna', action='store',
                     default='RX2',
                     dest='rx_antenna',
                     help='set RX antenna')
        add_argument('--tx-antenna', action='store',
                     default='TX/RX',
                     dest='tx_antenna',
                     help='set TX antenna')
        add_argument('--phy', action='store',
                     choices=['flexframe', 'ofdm', 'multiofdm'],
                     default='flexframe',
                     dest='phy',
                     help='set PHY')
        add_argument('-f', '--frequency', action='store', type=float,
                     default=3e9,
                     dest='frequency',
                     help='set center frequency (Hz)')
        add_argument('-b', '--bandwidth', action='store', type=float,
                     default=5e6,
                     dest='bandwidth',
                     help='set bandwidth (Hz)')
        add_argument('-g', '--soft-tx-gain', action='store', type=float,
                     default=-12,
                     dest='soft_tx_gain',
                     help='set soft TX gain (dB)')
        add_argument('-G', '--tx-gain', action='store', type=float,
                     default=25,
                     dest='tx_gain',
                     help='set UHD TX gain (dB)')
        add_argument('-R', '--rx-gain', action='store', type=float,
                     default=25,
                     dest='rx_gain',
                     help='set UHD RX gain (dB)')
        add_argument('--auto-soft-tx-gain', action='store_const', const=True,
                     default=False,
                     dest='auto_soft_tx_gain',
                     help='automatically choose soft TX gain')
        add_argument('-M', '--subcarriers', action='store', type=int,
                     default=48,
                     dest='M',
                     help='set number of OFDM subcarriers')
        add_argument('-C', '--cp', action='store', type=int,
                     default=6,
                     dest='cp_len',
                     help='set OFDM cyclic prefix length')
        add_argument('-T', '--taper', action='store', type=int,
                     default=4,
                     dest='taper_len',
                     help='set OFDM taper length')
        add_argument('-m', '--mod',
                     action='store', type=dragonradio.ModulationScheme,
                     default='qpsk',
                     dest='ms',
                     help='set modulation scheme: ' + enumHelp(dragonradio.ModulationScheme))
        add_argument('-c', '--fec0',
                     action='store', type=dragonradio.FECScheme,
                     default='v29',
                     dest='fec0',
                     help='set inner FEC: ' + enumHelp(dragonradio.FECScheme))
        add_argument('-k', '--fec1',
                     action='store', type=dragonradio.FECScheme,
                     default='rs8',
                     dest='fec1',
                     help='set outer FEC: ' + enumHelp(dragonradio.FECScheme))
        add_argument('-r', '--check',
                     action='store', type=dragonradio.CRCScheme,
                     default='crc32',
                     dest='check',
                     help='set data validity check: ' + enumHelp(dragonradio.CRCScheme))
        add_argument('--arq', action='store_const', const=True,
                     default=False,
                     dest='arq',
                     help='enable ARQ')

class Radio(object):
    def __init__(self, config):
        self.config = config
        self.node_id = config.node_id
        self.logger = None

        # Copy configuration settings to the C++ RadioConfig object
        for attr in ['verbose', 'soft_tx_gain', 'ms' ,'check', 'fec0', 'fec1']:
            if hasattr(config, attr):
                setattr(dragonradio.rc, attr, getattr(config, attr))

        # Create the USRP
        self.usrp = dragonradio.USRP(config.addr,
                                     config.frequency,
                                     config.tx_antenna,
                                     config.rx_antenna,
                                     config.tx_gain,
                                     config.rx_gain)

        # Create the logger *after* we create the USRP so that we have a global
        # clock
        logdir = config.logdir
        if logdir:
            path = self.getRadioLogPath()

            self.logger = dragonradio.Logger(path)
            self.logger.setAttribute('node_id', self.node_id)
            self.logger.setAttribute('frequency', config.frequency)
            self.logger.setAttribute('soft_tx_gain', config.soft_tx_gain)
            self.logger.setAttribute('tx_gain', config.tx_gain)
            self.logger.setAttribute('rx_gain', config.rx_gain)
            self.logger.setAttribute('M', config.M)
            self.logger.setAttribute('cp_len', config.cp_len)
            self.logger.setAttribute('taper_len', config.taper_len)

            if hasattr(config, 'log_sources'):
                for source in config.log_sources:
                    setattr(self.logger, source, True)

            dragonradio.Logger.singleton = self.logger

        #
        # Configure the PHY
        #
        if config.phy == 'flexframe':
            self.phy = dragonradio.FlexFrame(config.min_packet_size)
        elif config.phy == 'ofdm':
            self.phy = dragonradio.OFDM(config.M,
                                        config.cp_len,
                                        config.taper_len,
                                        config.min_packet_size)
        elif config.phy == 'multiofdm':
            self.phy = dragonradio.MultiOFDM(config.M,
                                             config.cp_len,
                                             config.taper_len,
                                             config.min_packet_size)
        else:
            fail('Bad PHY: {}'.format(config.phy))

        #
        # Create tun/tap interface and net neighborhood
        #
        self.tuntap = dragonradio.TunTap('tap0', False, 1500, '10.10.10.%d', 'c6:ff:ff:ff:ff:%02x', self.node_id)

        self.net = dragonradio.Net(self.tuntap, self.node_id)

        #
        # Configure the modulator and demodulator
        #
        self.modulator = dragonradio.ParallelPacketModulator(self.net,
                                                             self.phy,
                                                             config.num_modulation_threads)

        self.demodulator = dragonradio.ParallelPacketDemodulator(self.net,
                                                                 self.phy,
                                                                 config.num_demodulation_threads)

        #
        # Configure the controller
        #
        if config.arq:
            self.controller = dragonradio.SmartController(self.net,
                                                          config.arq_window,
                                                          config.arq_window)
        else:
            self.controller = dragonradio.DummyController(self.net)

        #
        # Configure packet path from demodulator to tun/tap
        # Right now, the path is direct:
        #   demodulator -> controller -> tun/tap
        #
        self.demodulator.source >> self.controller.radio_in

        self.controller.radio_out >> self.tuntap.sink

        #
        # Configure packet path from tun/tap to the modulator
        # The path is:
        #   tun/tap -> NetFilter -> NetQueue -> controller -> modulator
        #
        self.netfilter = dragonradio.NetFilter(self.net)

        self.netq = dragonradio.NetQueue()

        self.tuntap.source >> self.netfilter.input

        self.netfilter.output >> self.netq.push

        self.netq.pop >> self.controller.net_in

        self.controller.net_out >> self.modulator.sink

        #
        # If we are using a SmartController, tell it that the network queue is a
        # splice queue so that it can splice packets at the front of the queue.
        #
        if config.arq:
            self.controller.splice_queue = self.netq

    def configureALOHA(self):
        self.mac = dragonradio.SlottedALOHA(self.usrp,
                                            self.phy,
                                            self.modulator,
                                            self.demodulator,
                                            self.config.bandwidth,
                                            self.config.slot_size,
                                            self.config.guard_size,
                                            self.config.aloha_prob)

        if self.logger:
            self.logger.setAttribute('tx_bandwidth', self.usrp.tx_rate)
            self.logger.setAttribute('rx_bandwidth', self.usrp.rx_rate)

    def configureTDMA(self, nslots):
        self.mac = dragonradio.TDMA(self.usrp,
                                    self.phy,
                                    self.modulator,
                                    self.demodulator,
                                    self.config.bandwidth,
                                    self.config.slot_size,
                                    self.config.guard_size,
                                    nslots)

        if self.logger:
            self.logger.setAttribute('tx_bandwidth', self.usrp.tx_rate)
            self.logger.setAttribute('rx_bandwidth', self.usrp.rx_rate)

    def getRadioLogPath(self):
        """
        Determine where the HDF5 log file created by the low-level radio will
        live.
        """
        path = os.path.join(self.config.logdir, 'radio.h5')
        if not os.path.exists(path):
            return path

        # If the radio log exists, create a new one.
        i = 1
        while True:
            path = os.path.join(self.config.logdir, 'radio-{:02d}.h5'.format(i))
            if not os.path.exists(path):
                return path
            i += 1