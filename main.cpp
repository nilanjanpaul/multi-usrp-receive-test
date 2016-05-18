//
// Copyright 2011 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <complex>
#include <uhd/transport/udp_simple.hpp>
#include <fstream>
#include <sstream>
#include "CTimer.h"

#define DBG_OUT(x) std::cout << #x << " = " << x << std::endl

namespace po = boost::program_options;

int UHD_SAFE_MAIN(int argc, char *argv[]){
    uhd::set_thread_priority_safe();

    //variables to be set by po
    std::string args, sync, subdev, channel_list, ant;
    double seconds_in_future;
    size_t total_num_samps;
    double rate, freq, gain;
    std::string udp_dst_addr, port, filename_prefix;

    //setup the program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "single uhd device address args")
        ("secs", po::value<double>(&seconds_in_future)->default_value(1.5), "number of seconds in the future to receive")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(10000), "total number of samples to receive")
        ("freq", po::value<double>(&freq)->default_value(900e6), "RF center frequency in Hz for all channels")
        ("rate", po::value<double>(&rate)->default_value(100e6/16), "rate of incoming samples for all channels")
        ("gain", po::value<double>(&gain)->default_value(0), "gain for the RF chain for all channels")
        ("ant", po::value<std::string>(&ant), "rx antenna selection for all channels")
        ("prefix", po::value<std::string>(&filename_prefix)->default_value(""), "enables file output with filename prefix")
        ("addr", po::value<std::string>(&udp_dst_addr)->default_value(""), "udp address: 10.10.0.10")
        ("port", po::value<std::string>(&port)->default_value("1337"), "udp port: 1337")
        ("sync", po::value<std::string>(&sync)->default_value("now"), "synchronization method: now, pps, mimo")
        ("subdev", po::value<std::string>(&subdev), "subdev spec (homogeneous across motherboards)")
        ("dilv", "specify to disable inner-loop verbose")
        ("int-n", "tune USRP with integer-N tuning")
        ("channels", po::value<std::string>(&channel_list)->default_value("0"), "which channel(s) to use (specify \"0\", \"1\", \"0,1\", etc)")
    ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    //print the help message
    if (vm.count("help")){
        std::cout << boost::format("UHD RX Multi Receive %s") % desc << std::endl;
        std::cout <<
        "    This is a demonstration of how to receive aligned data from multiple channels.\n"
        "    This example can receive from multiple DSPs, multiple motherboards, or both.\n"
        "    The MIMO cable or PPS can be used to synchronize the configuration. See --sync\n"
        "\n"
        "    Specify --subdev to select multiple channels per motherboard.\n"
        "      Ex: --subdev=\"A:0 B:0\" to get 2 channels on a Basic RX.\n"
        "\n"
        "    Specify --args to select multiple motherboards in a configuration.\n"
        "      Ex: --args=\"addr0=192.168.10.2, addr1=192.168.10.3\"\n"
        << std::endl;
        return ~0;
    }

    
    bool verbose = vm.count("dilv") == 0;

    //create a usrp device
    std::cout << std::endl;
    std::cout << boost::format("Creating the usrp device with: %s...") % args << std::endl;
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);

    //always select the subdevice first, the channel mapping affects the other settings
    if (vm.count("subdev")) usrp->set_rx_subdev_spec(subdev); //sets across all mboards

    std::cout << boost::format("Using Device: %s") % usrp->get_pp_string() << std::endl;

    unsigned int num_mboards = usrp->get_num_mboards();
    unsigned int num_rx_channels = usrp->get_rx_num_channels();

    std::cout << "Number mboards        = " << num_mboards << std::endl;
    std::cout << "Number of rx channels = " << num_rx_channels << std::endl;

    //set the rx sample rate (sets across all channels)
    std::cout << boost::format("Setting RX Rate: %f Msps...") % (rate/1e6) << std::endl;
    usrp->set_rx_rate(rate);
    std::cout << boost::format("Actual RX Rate: %f Msps...") % (usrp->get_rx_rate()/1e6) << std::endl << std::endl;

    //set the rx center frequency
    std::cout << boost::format("Setting RX Freq: %f MHz...") % (freq/1e6) << std::endl;
    uhd::tune_request_t tune_request(freq);
    if(vm.count("int-n")) tune_request.args = uhd::device_addr_t("mode_n=integer");
    for (unsigned int ch = 0; ch < num_rx_channels; ++ch)
      usrp->set_rx_freq(tune_request, ch);
    std::cout << boost::format("Actual RX Freq: %f MHz...") % (usrp->get_rx_freq()/1e6) << std::endl << std::endl;

    //set the rx rf gain
    std::cout << boost::format("Setting RX Gain: %f dB...") % gain << std::endl;
    for (unsigned int ch = 0; ch < num_rx_channels; ++ch)
      usrp->set_rx_gain(gain,ch);
    std::cout << boost::format("Actual RX Gain: %f dB...") % usrp->get_rx_gain() << std::endl << std::endl;




    std::cout << boost::format("Setting device timestamp to 0...") << std::endl;
    if (sync == "now"){
        //This is not a true time lock, the devices will be off by a few RTT.
        //Rather, this is just to allow for demonstration of the code below.
        usrp->set_time_now(uhd::time_spec_t(0.0));
    }
    else if (sync == "pps"){
        usrp->set_time_source("external");
        usrp->set_time_unknown_pps(uhd::time_spec_t(0.0));
        boost::this_thread::sleep(boost::posix_time::seconds(1)); //wait for pps sync pulse
    }
    else if (sync == "mimo"){
        UHD_ASSERT_THROW(usrp->get_num_mboards() == 2);

        //make mboard 1 a slave over the MIMO Cable
        usrp->set_clock_source("mimo", 1);
        usrp->set_time_source("mimo", 1);

        //set time on the master (mboard 0)
        usrp->set_time_now(uhd::time_spec_t(0.0), 0);

        //sleep a bit while the slave locks its time to the master
        boost::this_thread::sleep(boost::posix_time::milliseconds(100));
    }

    //detect which channels to use
    std::vector<std::string> channel_strings;
    std::vector<size_t> channel_nums;
    boost::split(channel_strings, channel_list, boost::is_any_of("\"',"));
    for(size_t ch = 0; ch < channel_strings.size(); ch++){
        size_t chan = boost::lexical_cast<int>(channel_strings[ch]);
        if(chan >= usrp->get_rx_num_channels()){
            throw std::runtime_error("Invalid channel(s) specified.");
        }
        else channel_nums.push_back(boost::lexical_cast<int>(channel_strings[ch]));
    }
    DBG_OUT( channel_nums.size() );

    //create a receive streamer
    //linearly map channels (index0 = channel0, index1 = channel1, ...)
    uhd::stream_args_t stream_args("fc32"); //complex floats
    stream_args.channels = channel_nums;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    //setup streaming
    std::cout << std::endl;
    std::cout << boost::format(
        "Begin streaming %u samples, %f seconds in the future..."
    ) % total_num_samps % seconds_in_future << std::endl;
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps = total_num_samps;
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = uhd::time_spec_t(seconds_in_future);
    rx_stream->issue_stream_cmd(stream_cmd); //tells all channels to stream

    //meta-data will be filled in by recv()
    uhd::rx_metadata_t md;

    typedef std::vector<std::vector<std::complex<float> > > MultiDeviceBufferType;
    MultiDeviceBufferType MultiDeviceBuffer;
    MultiDeviceBuffer.resize( num_rx_channels );

    //allocate buffers to receive with samples (one buffer per channel)
    const size_t samps_per_buff = rx_stream->get_max_num_samps();
    std::vector<std::vector<std::complex<float> > > buffs(
        num_rx_channels, std::vector<std::complex<float> >(samps_per_buff)
    );


    //create a vector of pointers to point to each of the channel buffers
    std::vector<std::complex<float> *> buff_ptrs;
    for (size_t i = 0; i < buffs.size(); i++) buff_ptrs.push_back(&buffs[i].front());
    DBG_OUT( buff_ptrs.size() );

    //the first call to recv() will block this many seconds before receiving
    double timeout = seconds_in_future + 0.1; //timeout (delay before receive + padding)

    size_t num_acc_samps = 0; //number of accumulated samples
    while(num_acc_samps < total_num_samps){
        //receive a single packet
        size_t num_rx_samps = rx_stream->recv( buff_ptrs, samps_per_buff, md, timeout);

	// copy samples from buff_ptrs to MultiDeviceBuffer
	for (unsigned int i = 0; i < num_rx_channels; ++i)
	{
	  MultiDeviceBuffer.at(i).insert( MultiDeviceBuffer.at(i).end(), buffs.at(i).begin(), buffs.at(i).end() );
	}

        //use a small timeout for subsequent packets
        timeout = 0.1;

        //handle the error code
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) break;
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE){
            throw std::runtime_error(str(boost::format(
                "Receiver error %s"
            ) % md.strerror()));
        }

        if(verbose) std::cout << boost::format(
            "Received packet: %u samples, %u full secs, %f frac secs"
        ) % num_rx_samps % md.time_spec.get_full_secs() % md.time_spec.get_frac_secs() << std::endl;

        num_acc_samps += num_rx_samps;
    }

    // check size
    for (unsigned int i = 0; i < num_rx_channels; ++i)
      DBG_OUT( MultiDeviceBuffer.at(i).size() );

    if (num_acc_samps < total_num_samps) 
    {
      std::cerr << "Receive timeout before all samples received..." << std::endl;
      return 0;
    }


    if (filename_prefix != "") // file output
    {
      unsigned int tbw = 0;
      CTimer tmr("Write file buffer: ");
      for (unsigned int ch = 0; ch < num_rx_channels; ++ch)
      {
	uhd::dict<std::string, std::string> usrp_info = usrp->get_usrp_rx_info(ch);
	std::stringbuf buffer;
	std::ostream os(&buffer);

	os << "channel              : " << ch << std::endl;
	os << "mboard id            : " << usrp_info["mboard_id"] << std::endl;
	os << "mboard_serial        : " << usrp_info["mboard_serial"] << std::endl;
	os << "mboard_name          : " << usrp_info["mboard_name"] << std::endl;
	os << "rx_id                : " << usrp_info["rx_id"] << std::endl;
	os << "rx_subdev_name       : " << usrp_info["rx_subdev_name"] << std::endl;
	os << "rx_subdev_spec       : " << usrp_info["rx_subdev_spec"] << std::endl;
	os << "total samples        : " << total_num_samps << std::endl;
	os << "sample size          : " << sizeof(MultiDeviceBuffer.at(ch).at(0)) << std::endl;
	os << "RX frequency (MHz)   : " << usrp->get_rx_freq(ch)/1e6 << std::endl;
	os << "RX sample rate (MHz) : " << usrp->get_rx_rate(ch)/1e6 << std::endl;
	os << "RX gain (dB)         : " << usrp->get_rx_gain(ch) << std::endl;
	os << "RX antenna           : " << usrp->get_rx_antenna(ch) << std::endl;
	//std::cout << buffer.str();


	std::string outfilename;
	outfilename = filename_prefix + "ch_" + boost::lexical_cast<std::string>(ch) + "_meta";

	// ascii file output with meta information
	std::ofstream ofs(outfilename.c_str());
	ofs << buffer.str();

	if (total_num_samps < 16000)
	  for (unsigned int i = 0; i < total_num_samps; ++i)
	    ofs << i << " " << MultiDeviceBuffer.at(ch).at(i).real() << " " << MultiDeviceBuffer.at(ch).at(i).imag() << std::endl; 

	ofs.close();

	// binary file output with meta information
	outfilename = filename_prefix + "ch_" + boost::lexical_cast<std::string>(ch) + "_binary";
	std::ofstream ofs_binary(outfilename.c_str(), std::ofstream::binary);

	tbw += sizeof(MultiDeviceBuffer.at(ch).at(0)) * total_num_samps;

	ofs_binary.write((char *)MultiDeviceBuffer.at(ch).data(), sizeof(MultiDeviceBuffer.at(ch).at(0)) * total_num_samps );
	ofs_binary.close();
      }
      std::cout << "Total written: " << tbw / 1024.0 / 1024.0 << " MB" << std::endl;
    }


    if (udp_dst_addr != "")  // udp output
    {
      std::cout << "Sending samples to " << udp_dst_addr << ":" << port << std::endl;
      // send out samples via udp_xport

      uhd::transport::udp_simple::sptr udp_xport = uhd::transport::udp_simple::make_connected(udp_dst_addr, port);

      udp_xport->send(boost::asio::buffer( &num_rx_channels , 4 ));

      udp_xport->send(boost::asio::buffer( &total_num_samps , 4 ));

      unsigned int num_samps_per_datagram;
      //num_samps_per_datagram = (uhd::transport::udp_simple::mtu) / sizeof(std::complex<float>);
      num_samps_per_datagram = 256;

      // DEBUG replace floor with ceil
      unsigned int num_udp_datagrams = floor((float)total_num_samps / (float)(num_samps_per_datagram));
      udp_xport->send(boost::asio::buffer( &num_udp_datagrams , 4 ));


      for (unsigned int channel = 0; channel < num_rx_channels; ++channel)
	{
	  std::complex<float> *ptr_to_device_buffer = (std::complex<float> *)MultiDeviceBuffer.at(channel).data();
	  for (unsigned int i = 0; i < num_udp_datagrams; ++i)
	    {
	      udp_xport->send(boost::asio::buffer((void*)ptr_to_device_buffer,
						  num_samps_per_datagram * sizeof(std::complex<float>) )
			      );
	      ptr_to_device_buffer += num_samps_per_datagram;
	      boost::this_thread::sleep(boost::posix_time::milliseconds(10));
	    }
	}
    }

    //finished
    std::cout << std::endl << "Done!" << std::endl << std::endl;

    return EXIT_SUCCESS;
}
