/* -*- c++ -*- */
/*
 * Copyright 2015 <+YOU OR YOUR COMPANY+>.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include <pmt/pmt.h>
#include <gnuradio/prefs.h>
#include "level_capture_trigger_impl.h"

//#define IQCAPTURE_DEBUG
namespace gr {
namespace msod_sensor {

level_capture_trigger::sptr
level_capture_trigger::make(size_t itemsize, int level, size_t window_size)
{
    return gnuradio::get_initial_sptr
           (new level_capture_trigger_impl(itemsize,level,window_size));
}

/*
 * The private constructor
 */
level_capture_trigger_impl::level_capture_trigger_impl(size_t itemsize, int level, size_t window_size)
    : gr::block("level_capture_trigger",
                     gr::io_signature::make(1, 1, itemsize),
                     gr::io_signature::make(1, 1, itemsize))
{
    this->d_level = level;
    this->d_window_size = window_size;
    this->d_itemcount = 0;
    this->d_itemsize = itemsize;
    this->d_logging_enabled = true;
    // Shared memory because this is signalled from a separate process that reads commands from the server.
    this->d_armed = new boost::interprocess::mapped_region(boost::interprocess::anonymous_shared_memory(sizeof(int)));
    memset(d_armed->get_address(), 0, d_armed->get_size());
    message_port_register_out(pmt::mp("trigger"));
#ifdef IQCAPTURE_DEBUG
    prefs *p = prefs::singleton();
    std::string log_level = p->get_string("LOG", "log_level", "debug");
    GR_LOG_SET_LEVEL(d_debug_logger,log_level);
#else
    prefs *p = prefs::singleton();
    std::string log_level = p->get_string("LOG", "log_level", "info");
    GR_LOG_SET_LEVEL(d_debug_logger,log_level);
#endif
}

/*
 * Our virtual destructor.
 */
level_capture_trigger_impl::~level_capture_trigger_impl()
{
}

 void
level_capture_trigger_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
{
         ninput_items_required[0] = noutput_items;
}

bool
level_capture_trigger_impl::is_armed() {
    int armed;
    memcpy(&armed,this->d_armed->get_address(),sizeof(int));
    return armed;
}

void
level_capture_trigger_impl::arm() {
        memset(d_armed->get_address(), 1, sizeof(int));
        GR_LOG_DEBUG(d_debug_logger,"level_capture_trigger::arm " + std::to_string((long) this ) + " arm_flag " + std::to_string(this->is_armed()));
}

void
level_capture_trigger_impl::disarm() {
        GR_LOG_DEBUG(d_debug_logger,"level_capture_trigger::disarm " );
        memset(d_armed->get_address(), 0, sizeof(int));
}


int
level_capture_trigger_impl::general_work (int noutput_items,
                              gr_vector_int &ninput_items,
                              gr_vector_const_void_star &input_items,
                              gr_vector_void_star &output_items)

{
    const char *in = (const char *) input_items[0];
    char *out = (char *) output_items[0];
    unsigned int byte_size = noutput_items * this->d_itemsize;
    this->d_itemcount = this->d_itemcount + noutput_items;
    // Capture the window. Find the max power in the window. 
    // If this power exceeds a threshold then signal.
    // GR_LOG_DEBUG(d_debug_logger,"level_capture_trigger::ninput_items " + std::to_string(noutput_items) + " itemsize " + std::to_string(d_itemsize) );
    
    if (this->is_armed()) {
	// TODO-- this assumes float32 inputs.
        for(int i = 0; i< noutput_items; i ++) {
	   float ivalue = (float) *(in + i*d_itemsize);
	   float qvalue = (float) *(in + i*d_itemsize + 4);
	   float power = ivalue*ivalue + qvalue*qvalue;
	   if (power > d_level) {
       	      message_port_pub(pmt::mp("trigger"),pmt::intern(std::string("start")));
       	      GR_LOG_DEBUG(d_debug_logger,"level_capture_trigger::work pub" );
	      break;
	   } else {
	      if (this->d_logging_enabled ) {
       	          GR_LOG_DEBUG(d_debug_logger,"level_capture_trigger:: power level " + std::to_string(power)  );
	      }
	   }
	}
	this->disarm();
	this->d_logging_enabled = false;
    }

    in = (char*)input_items[0];
    byte_size = noutput_items * d_itemsize;
    memcpy(out,in,byte_size);
    consume_each (noutput_items);
    return noutput_items;
}

} /* namespace msod_sensor */
} /* namespace gr */

