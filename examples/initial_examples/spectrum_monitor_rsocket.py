#!/usr/bin/env python
#
# Copyright 2005,2007,2011 Free Software Foundation, Inc.
#
# This file is part of GNU Radio
#
# GNU Radio is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# GNU Radio is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Radio; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

from gnuradio import gr, eng_notation
from gnuradio import blocks
from gnuradio import filter
from gnuradio import fft
from gnuradio import uhd
from gnuradio.eng_option import eng_option
from optparse import OptionParser
import sys
import math
import threading
import myblocks
import array
import time
import json
import myjsocket as jsocket
import requests
from timezone import getLocalUtcTimeStamp, formatTimeStampLong

class Struct(dict):
    def __init__(self, **kwargs):
	super(Struct, self).__init__(**kwargs)
	self.__dict__ = self

class ThreadClass(threading.Thread):
    def run(self):
        return

class my_top_block(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self)

        usage = "usage: %prog [options] center_freq band_width"
        parser = OptionParser(option_class=eng_option, usage=usage)
        parser.add_option("-a", "--args", type="string", default="",
                          help="UHD device device address args [default=%default]")
        parser.add_option("", "--spec", type="string", default=None,
	                  help="Subdevice of UHD device where appropriate")
        parser.add_option("-A", "--antenna", type="string", default=None,
                          help="select Rx Antenna where appropriate")
        parser.add_option("-s", "--samp-rate", type="eng_float", default=1e6,
                          help="set sample rate [default=%default]")
        parser.add_option("-g", "--gain", type="eng_float", default=None,
                          help="set gain in dB (default is midpoint)")
        parser.add_option("", "--meas-interval", type="eng_float",
                          default=0.1, metavar="SECS",
                          help="interval over which to measure statistic (in seconds) [default=%default]")
        parser.add_option("-c", "--number-channels", type="int", default=100, 
                          help="number of uniform channels for which to report power measurements [default=%default]")
        parser.add_option("-l", "--lo-offset", type="eng_float",
                          default=0, metavar="Hz",
                          help="lo_offset in Hz [default=half the sample rate]")
        parser.add_option("-F", "--fft-size", type="int", default=1024,
                          help="specify number of FFT bins [default=%default]")
        parser.add_option("", "--real-time", action="store_true", default=False,
                          help="Attempt to enable real-time scheduling")
    	parser.add_option("-d", "--dest-host", type="string", default="",
                          help="set destination host for sending data")
        parser.add_option("", "--skip-DC", action="store_true", default=False,
                          help="skip the DC bin when mapping channels")

        (options, args) = parser.parse_args()
        if len(args) != 2:
            parser.print_help()
            sys.exit(1)

	self.center_freq = eng_notation.str_to_num(args[0])
	self.bandwidth = eng_notation.str_to_num(args[1])

        if not options.real_time:
            realtime = False
        else:
            # Attempt to enable realtime scheduling
            r = gr.enable_realtime_scheduling()
            if r == gr.RT_OK:
                realtime = True
            else:
                realtime = False
                print "Note: failed to enable realtime scheduling"

        # build graph
        self.u = uhd.usrp_source(device_addr=options.args,
                                 stream_args=uhd.stream_args('fc32'))

        # Set the subdevice spec
        if(options.spec):
            self.u.set_subdev_spec(options.spec, 0)

        # Set the antenna
        if(options.antenna):
            self.u.set_antenna(options.antenna, 0)
        
        self.u.set_samp_rate(options.samp_rate)
        usrp_rate = self.u.get_samp_rate()

	if usrp_rate != options.samp_rate:
	    if usrp_rate < options.samp_rate:
	        # create list of allowable rates
	        samp_rates = self.u.get_samp_rates()
	        rate_list = [0.0]*len(samp_rates)
	        for i in range(len(rate_list)):
		    last_rate = samp_rates.pop()
		    rate_list[len(rate_list) - 1 - i] = last_rate.start()
		# choose next higher rate
		rate_ind = rate_list.index(usrp_rate) + 1
		if rate_ind < len(rate_list):
		    self.u.set_samp_rate(rate_list[rate_ind])
		    usrp_rate = self.u.get_samp_rate()
		print "New actual sample rate =", usrp_rate/1e6, "MHz"
	    resamp = filter.fractional_resampler_cc(0.0, usrp_rate / options.samp_rate)

	self.samp_rate = options.samp_rate
        
	if(options.lo_offset):
            self.lo_offset = options.lo_offset
	else:
	    self.lo_offset = usrp_rate / 2.0
	    print "LO offset set to", self.lo_offset/1e6, "MHz"

        self.fft_size = options.fft_size
        self.num_ch = options.number_channels
        
        s2v = blocks.stream_to_vector(gr.sizeof_gr_complex, self.fft_size)

        mywindow = filter.window.blackmanharris(self.fft_size)
        ffter = fft.fft_vcc(self.fft_size, True, mywindow, True)
        window_power = sum(map(lambda x: x*x, mywindow))

        c2mag = blocks.complex_to_mag_squared(self.fft_size)

	self.bin2ch_map = [0] * self.fft_size
        hz_per_bin = self.samp_rate / self.fft_size
	channel_bw = hz_per_bin * round(self.bandwidth / self.num_ch / hz_per_bin)
	self.bandwidth = channel_bw * self.num_ch
	print "Actual width of band is", self.bandwidth/1e6, "MHz."
	start_freq = self.center_freq - self.bandwidth/2.0
	stop_freq = start_freq + self.bandwidth
	for j in range(self.fft_size):
	    fj = self.bin_freq(j, self.center_freq)
	    if (fj >= start_freq) and (fj < stop_freq):
	        channel_num = int(math.floor((fj - start_freq) / channel_bw)) + 1
	        self.bin2ch_map[j] = channel_num
	if options.skip_DC:
	    self.bin2ch_map[(self.fft_size + 1) / 2 + 1:] = self.bin2ch_map[(self.fft_size + 1) / 2 : -1]
	    self.bin2ch_map[(self.fft_size + 1) / 2] = 0
	if self.bandwidth > self.samp_rate:
	    print "Warning: Width of band (" + str(self.bandwidth/1e6), "MHz) is greater than the sample rate (" + str(self.samp_rate/1e6), "MHz)."

	self.aggr = myblocks.bin_aggregator_ff(self.fft_size, self.num_ch, self.bin2ch_map)

        meas_frames = max(1, int(round(options.meas_interval * self.samp_rate / self.fft_size))) # in fft_frames
	self.meas_duration = meas_frames * self.fft_size / self.samp_rate
	print "Actual measurement duration =", self.meas_duration, "s"

        self.stats = myblocks.bin_statistics_ff(self.num_ch, meas_frames)

	# Divide magnitude-square by a constant to obtain power
	# in Watts.  Assumes unit of USRP source is volts.
	impedance = 50.0   # ohms
	Vsq2W_dB = -10.0 * math.log10(self.fft_size * window_power * impedance)

	# Convert from Watts to dBm.
	W2dBm = blocks.nlog10_ff(10.0, self.num_ch, 30.0 + Vsq2W_dB)

	f2c = blocks.float_to_char(self.num_ch, 1.0)

	self.dest_host = options.dest_host

	# file descriptor is set in main loop; use dummy value for now
	self.srvr = myblocks.file_descriptor_sink(self.num_ch * gr.sizeof_char, 0)

	if usrp_rate > self.samp_rate:
	    # insert resampler
	    self.connect(self.u, resamp, s2v)
	else:
	    self.connect(self.u, s2v)
	self.connect(s2v, ffter, c2mag, self.aggr, self.stats, W2dBm, f2c, self.srvr)
	#self.connect(s2v, ffter, c2mag, self.aggr, self.stats, W2dBm, self.srvr)

        g = self.u.get_gain_range()
        if options.gain is None:
            # if no gain was specified, use the mid-point in dB
            options.gain = float(g.start()+g.stop())/2.0

        self.set_gain(options.gain)
        print "gain =", options.gain, "dB in range (%0.1f dB, %0.1f dB)" % (float(g.start()), float(g.stop()))
	self.atten = float(g.stop()) - options.gain

    def set_freq(self, target_freq):
        """
        Set the center frequency we're interested in.

        Args:
            target_freq: frequency in Hz
        @rypte: bool
        """
        
        r = self.u.set_center_freq(uhd.tune_request(target_freq, rf_freq=(target_freq + self.lo_offset),rf_freq_policy=uhd.tune_request.POLICY_MANUAL))
        if r:
            return True

        return False

    def set_gain(self, gain):
        self.u.set_gain(gain)
    
    def bin_freq(self, i_bin, center_freq):
        hz_per_bin = self.samp_rate / self.fft_size
	# For odd fft_size, treats i_bin = (fft_size + 1) / 2 as the DC bin.
        freq = center_freq + hz_per_bin * (i_bin - self.fft_size / 2 - self.fft_size % 2)
        return freq
    
    def set_fd(self, fd):
        self.srvr.set_fd(fd)

    def set_bin2ch_map(self, bin2ch_map):
        self.aggr.set_bin_index(bin2ch_map)

    def read_json_from_file(self, fname):
	f = open(fname,'r')
	obj = json.load(f)
	f.close()
        return obj

def main_loop(tb):
    
    if not tb.set_freq(tb.center_freq):
        print "Failed to set frequency to", tb.center_freq
        sys.exit(1)
    print "Set frequency to", tb.center_freq/1e6, "MHz"
    time.sleep(0.25)

    # Establish connection to server
    r = requests.post('http://'+tb.dest_host+':8000/sensordata/getStreamingPort')
    response = r.json()
    print 'socket port =', response['port']
    tb.s = s = jsocket.JsonClient(address=tb.dest_host, port=response['port'])
    s.connect()
    tb.set_fd(s.conn.fileno())

    # Send location and system info to server
    loc_msg = tb.read_json_from_file('sensor.loc')
    sys_msg = tb.read_json_from_file('sensor.sys')
    ts = long(round(getLocalUtcTimeStamp()))
    sensor_id = tb.u.get_usrp_info()['rx_serial']
    print 'Serial no.', sensor_id
    loc_msg['t'] = ts
    loc_msg['SensorID'] = sensor_id 
    sys_msg['t'] = ts
    sys_msg['SensorID'] = sensor_id 
    s.send_obj(loc_msg)
    s.send_obj(sys_msg)

    # Form data header
    ts = long(round(getLocalUtcTimeStamp()))
    f_start = tb.center_freq - tb.bandwidth / 2.0
    f_stop = f_start + tb.bandwidth
    mpar = Struct(fStart=f_start, fStop=f_stop, n=tb.num_ch, td=-1, tm=tb.meas_duration, Det='Average', Atten=tb.atten)
    # Need to add a field for overflow indicator
    data = Struct(Ver='1.0.12', Type='Data', SensorID=sensor_id, SensorKey='NaN', t=ts, Sys2Detect='LTE', Sensitivity='Low', mType='FFT-Power', t1=ts, a=1, nM=-1, Ta=-1, OL='NaN', wnI=-77.0, Comment='Using hard-coded (not detected) system noise power for wnI', Processed='False', DataType = 'Binary - int8', ByteOrder='N/A', Compression='None', mPar=mpar)

    s.send_obj(data)
    date_str = formatTimeStampLong(ts, loc_msg['TimeZone'])
    print date_str, "fc =", tb.center_freq/1e6, "MHz. Sending data to", tb.dest_host

    # Execute flow graph and wait for it to stop
    tb.start()
    tb.wait()
    tb.s.close()
    print 'Closed socket'

if __name__ == '__main__':
    t = ThreadClass()
    t.start()

    tb = my_top_block()
    try:
        main_loop(tb)

    except KeyboardInterrupt:
        pass
