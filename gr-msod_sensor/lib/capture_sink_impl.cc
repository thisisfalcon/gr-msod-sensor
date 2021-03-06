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

//#define IQCAPTURE_DEBUG

#include <gnuradio/io_signature.h>
#include <gnuradio/prefs.h>
#include <gnuradio/logger.h>
#include <mongo/bson/bson.h>
#include <mongo/client/dbclient.h>
#include <pmt/pmt.h>
#include <ctime>
#include <fstream>
#include <iostream>
#include <exception>
#undef NDEBUG
#include <cassert>
#include "capture_sink_impl.h"
#include <curl/curl.h>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/interprocess/mapped_region.hpp>





namespace gr {
namespace msod_sensor {


capture_sink::sptr
capture_sink::make(size_t itemsize, size_t chunksize, size_t samp_rate, char* capture_dir, int mongodb_port, char* event_url, int time_offset)
{
    return gnuradio::get_initial_sptr
           (new capture_sink_impl(itemsize, chunksize, samp_rate, capture_dir,mongodb_port,event_url,time_offset));
}

/*
 * The private constructor
 */
capture_sink_impl::capture_sink_impl(size_t itemsize, size_t chunksize, size_t samp_rate, char* capture_dir, int mongodb_port, char* event_url, int time_offset)
    :gr::sync_block("capture_sink",
                    gr::io_signature::make(1, 1, itemsize),
                    gr::io_signature::make(0, 0, 0))
{
    prefs *p = prefs::singleton();
#ifdef IQCAPTURE_DEBUG
    std::string log_level = p->get_string("LOG", "log_level", "debug");
#else
    std::string log_level = p->get_string("LOG", "log_level", "info");
#endif
    GR_LOG_SET_LEVEL(d_debug_logger,log_level);
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl:: itemsize = " + std::to_string(itemsize) + " chunksize = " + std::to_string(chunksize) +
                 " capture_dir = "  + capture_dir  );

    d_start_capture = new boost::interprocess::mapped_region(boost::interprocess::anonymous_shared_memory(sizeof(int)));
    d_time_offset = time_offset;
    d_itemsize = itemsize;
    d_samp_rate = samp_rate;
    d_capture_dir = new char[strlen(capture_dir) + 1];
    strcpy(d_capture_dir,capture_dir);
    d_chunksize = chunksize;
    d_itemcount = 0;
    d_current_capture_file = NULL;
    d_capture_buffer = new gr_complex[chunksize];
    memset(d_capture_buffer,0,sizeof(d_capture_buffer));
    memset(d_start_capture->get_address(), 0, d_start_capture->get_size());
    d_event_url = new char[strlen(event_url) + 1];
    strcpy(d_event_url,event_url);
    std::string errmsg;
    try {
        if (!d_mongo_client.connect(std::string("127.0.0.1:") + std::to_string(mongodb_port) ,errmsg)) {
            GR_LOG_ERROR(d_debug_logger,"failed to initialize the client driver");
            throw std::runtime_error("cannot connect to Mongo Client");
        }
        GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl:: connected to mongod ");
    } catch (std::exception& e) {
        GR_LOG_ERROR(d_debug_logger,"Unexpected exception");
        throw e;
    }
    message_port_register_in(pmt::mp("capture"));
    set_msg_handler(pmt::mp("capture"),boost::bind(&gr::msod_sensor::capture_sink_impl::message_handler,this, _1));
}

/*
 * Our virtual destructor.
 */
capture_sink_impl::~capture_sink_impl()
{

}

/*
* Generate a file name (timestamped).
*/
time_t capture_sink_impl::generate_timestamp() {
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::generate_timestamp ");
    struct timeval tp;
    gettimeofday(&tp, NULL);
    time_t  timev = tp.tv_sec;
    std::string* dirname = new std::string(d_capture_dir);
    dirname->append("/capture-");
    std::string time_stamp = std::to_string(timev);
    dirname->append(time_stamp);
    struct stat statbuf;
    if ( stat(dirname->c_str(),&statbuf) != -1 ) {
        for (int counter = 1 ; counter < 1000; counter++) {
            std::string* temp_dirname = new  std::string(*dirname);
            temp_dirname->append(".");
            temp_dirname->append(std::to_string(counter));
            if ( stat(temp_dirname->c_str(),&statbuf) == -1 ) {
                delete dirname;
                dirname = temp_dirname;
                break;
            } else {
                delete temp_dirname;
            }
        }
    }
    if ( d_current_capture_file != NULL) {
        delete d_current_capture_file;
    }
    d_current_capture_file =  dirname;
    return timev;
}

void
capture_sink_impl::message_handler(pmt::pmt_t msg) {
#ifdef IQCAPTURE_DEBUG
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::capture ");
#endif
    //Write all the memory to 1
    memset(d_start_capture->get_address(), 1, sizeof(int));
}

/**
* synchronous invocation that sets the capture flag. Invocable through python api.
*/

void
capture_sink_impl::start_capture() {
#ifdef IQCAPTURE_DEBUG
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::start_capture ");
#endif
    memset(d_start_capture->get_address(), 1, sizeof(int));
}

void
capture_sink_impl::stop_capture() {
#ifdef IQCAPTURE_DEBUG
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::stop_capture ");
#endif
    memset(d_start_capture->get_address(), 0, sizeof(int));
}


void
capture_sink_impl::set_event_message(char* event_message) {
    try {
        d_event_message = mongo::fromjson(std::string(event_message));
        // The "t" field is updated later when the message is POSTed.
        d_event_message.removeField("t");
    } catch ( mongo::DBException& e) {
        GR_LOG_ERROR(d_debug_logger,"failed to initialize the client driver");
        throw std::runtime_error("Invalid data message");
    }
}


/**
* Dump the existing buffer into a file and push a record into mongodb.
*/

bool
capture_sink_impl::dump_buffer() {
    int buffercounter = 0;
    time_t ts = generate_timestamp();
    GR_LOG_ERROR(d_debug_logger,"capture_sink_impl::dump_buffer: logging to  : " + *d_current_capture_file );
    assert(d_itemcount == d_chunksize);
    int fd = open(d_current_capture_file->c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd < 0 ) {
        GR_LOG_ERROR(d_debug_logger,"capture_sink_impl::dump_buffer: open failed on : " + *d_current_capture_file->c_str());
        return false;
    }
    size_t written = write(fd,d_capture_buffer,d_chunksize*sizeof(gr_complex));
    close(fd);
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::dump_buffer: wrote " + std::to_string(written) + " elements to file : " + *d_current_capture_file);
    time_t universal_timestamp = ts + d_time_offset;
    mongo::BSONObjBuilder builder;
    builder.appendElements(d_event_message);
    mongo::BSONObj event_message = builder.appendNumber("t",(long long) universal_timestamp)
                                   .appendNumber("SampleCount",(long long) d_itemcount)
                                   .obj();


#ifdef IQCAPTURE_DEBUG
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::event_message " + event_message.toString());
#endif
    // Send a message to MSOD indicating capture event.
    CURL *curl;
    // get a curl handle
    curl = curl_easy_init();
    if(curl) {
        GR_LOG_DEBUG(d_debug_logger,"capture_sink_imp:: POSTING to d_event_url : " + std::string(d_event_url))
        GR_LOG_DEBUG(d_debug_logger,"capture_sink_imp:: event_url body : " + event_message.jsonString())
        char* message_body = new char[strlen(event_message.jsonString().c_str()) + 1];
        strcpy(message_body,event_message.jsonString().c_str());
        struct curl_slist *slist=NULL;
        slist = curl_slist_append(slist, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
        curl_easy_setopt(curl, CURLOPT_POST, 1);
        curl_easy_setopt(curl, CURLOPT_URL, d_event_url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, message_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(event_message.jsonString().c_str()));
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // TODO -- enable this check after official cert is installed.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L); // TODO -- make this 2L for strict check after official cert installed.
        CURLcode res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            GR_LOG_ERROR(d_debug_logger,"Curl POST not successful : " + std::string(curl_easy_strerror(res)));
        }
        delete message_body;
        curl_easy_cleanup(curl);
    } else {
        GR_LOG_ERROR(d_debug_logger,"Curl initialization not successful");
        return false;
    }

    // insert the message into the local database.
    mongo::BSONObjBuilder builder1;
    builder1.appendElements(d_event_message);
    // Add the file name here -- it is not relevant to the server.
    event_message = builder1.append("_capture_file",*d_current_capture_file)
                    .appendNumber("t",(long long) universal_timestamp)
                    .appendNumber("SampleCount",(long long) d_itemcount)
                    .obj();


    try {
        d_mongo_client.insert("iqcapture.dataMessages",event_message);
    } catch (mongo::DBException& e) {
        GR_LOG_ERROR(d_debug_logger,"capture_sink_impl::Error inserting into mongodb ");
        return false;
    }

    clear_buffer();
    return true;
}


void
capture_sink_impl::clear_buffer() {
    // Clear the capture vector. This also deletes the elements of the capture buffer.
    d_itemcount = 0;
}



int
capture_sink_impl::work(int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items)
{

    // Capture is enabled.
    const gr_complex *input = (const gr_complex *) input_items[0];
    // Capture is not enabled. Just pass through.
    int start_capture_flag;
    memcpy(&start_capture_flag,d_start_capture->get_address(),sizeof(int));
    if (!start_capture_flag) return noutput_items;
    unsigned int byte_size = noutput_items * d_itemsize;
#ifdef IQCAPTURE_DEBUG
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::work byte_size " + std::to_string(byte_size));
    GR_LOG_DEBUG(d_debug_logger,"capture_sink_impl::work noutput_items " + std::to_string(noutput_items));
#endif
    int buffercounter = 0;
    for (int i = 0; i < noutput_items; i++ , input++) {
        // Exceeded our storage capacity? So dump the buffer and clear it.
        if (d_itemcount ==  d_chunksize && start_capture_flag) {
            memset(d_start_capture->get_address(), 0, d_start_capture->get_size());
            if (! dump_buffer() ) {
                return -1;
            } else {
                return noutput_items;
            }
        }
        // Our queue is not yet full. Keep adding to it.
        d_capture_buffer[d_itemcount] = *input;
        d_itemcount++;
    }
    return noutput_items;
}

} /* namespace capture */
} /* namespace gr */

