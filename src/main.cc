// DWSL - full radio stack

#include <MAC.hh>
#include <NET.hh>
#include <PHY.hh>
#include <USRP.hh>
#include <stdio.h>
#include <unistd.h>
#include <thread>

void usage(void)
{
}

int main(int argc, char** argv)
{
    // TODO
    // make these things CLI configurable
    double center_freq = 1340e6;                // Hz
    double bandwidth = 5e6;                    // Hz
    unsigned int padded_bytes = 512;            // bytes to add to each paylaod
    float tx_gain = 25;                         // dB
    float rx_gain = 25;                         // dB
    unsigned int node_id = 1;                   // must be in {1,...,num_nodes_in_net}
    unsigned int num_nodes_in_net = 2;          // number of nodes in network
    double frame_size = .07;                     // slot_size*num_nodes_in_net (seconds)
    unsigned int rx_thread_pool_size = 4;       // number of threads available for demodulation
    float pad_size = .01;                       // inter slot dead time
    unsigned int packets_per_slot = 2;          // how many packets to stuff into each slot
    std::string addr;

    int ch;

    while ((ch = getopt(argc, argv, "a:n:")) != -1) {
      switch (ch) {
        case 'a':
          addr = optarg;
          break;

        case 'n':
          node_id = atoi(optarg);
          printf("node_id = %d\n", node_id);
          break;

        case '?':
        default:
        usage();
      }
    }

    argc -= optind;
    argv += optind;

    ///////////////////////////////////////////////////////////////////////////////////////

    std::vector<unsigned char> nodes_in_net(num_nodes_in_net);

    for(unsigned int i=0;i<num_nodes_in_net;i++)
    {
        nodes_in_net[i] = i+1;
    }

    std::shared_ptr<FloatIQTransport> t(new USRP(addr, center_freq, bandwidth, "TX/RX", "RX2", tx_gain, rx_gain));
    std::shared_ptr<NET>              net(new NET("tap0",node_id,nodes_in_net));
    std::shared_ptr<PHY>              phy(new PHY(t, net,padded_bytes,rx_thread_pool_size));
    std::shared_ptr<MAC>              mac(new MAC(t, net, phy,frame_size,pad_size,packets_per_slot));

    // use main thread for tx_worker
    mac->run();

    printf("Done\n");
}