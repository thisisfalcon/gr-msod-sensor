/* -*- c++ -*- */
/*
 * Copyright 2014 <+YOU OR YOUR COMPANY+>.
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

#include <algorithm> /* copy */

#include <gnuradio/io_signature.h>
#include <volk/volk.h>
#include "bin_statistics_ff_impl.h"

namespace gr {
namespace msod_sensor {

bin_statistics_ff::sptr
bin_statistics_ff::make(unsigned int vlen, unsigned int meas_interval, int det)
{
    return gnuradio::get_initial_sptr
           (new bin_statistics_ff_impl(vlen, meas_interval, det));
}

/*
 * The private constructor
 */
bin_statistics_ff_impl::bin_statistics_ff_impl(unsigned int vlen, unsigned int meas_interval, int det)
    : gr::sync_decimator("bin_statistics_ff",
                         gr::io_signature::make(1, 1, vlen * sizeof(float)),
                         gr::io_signature::make(1, 1, vlen * sizeof(float)), meas_interval),
    d_vlen(vlen), d_meas_interval(meas_interval), d_det(det)
{}

/*
 * Our virtual destructor.
 */
bin_statistics_ff_impl::~bin_statistics_ff_impl()
{
}

int
bin_statistics_ff_impl::work(int noutput_items,
                             gr_vector_const_void_star &input_items,
                             gr_vector_void_star &output_items)
{
    const float *in = (const float *) input_items[0];
    float *out = (float *) output_items[0];

    if (d_meas_interval == 1)
    {
        // No averaging required, copy all
        std::copy(in, &in[d_vlen * noutput_items], out);
    }
    else
    {
        // Apply statistic to the input vectors in the measurement interval
        for (size_t n = 0; n < noutput_items; ++n)
        {
            std::copy(&in[n * d_vlen * d_meas_interval],
                      &in[n * d_vlen * d_meas_interval + d_vlen],
                      &out[n * d_vlen]);

            for (size_t i = 1; i < d_meas_interval; ++i)
            {
                if (d_det == AVG) {
                    volk_32f_x2_add_32f(&out[n * d_vlen],
                                        &out[n * d_vlen],
                                        &in[(n * d_meas_interval + i) * d_vlen],
                                        d_vlen);
                } else if (d_det == PEAK) {
                    volk_32f_x2_max_32f(&out[n * d_vlen],
                                        &out[n * d_vlen],
                                        &in[(n * d_meas_interval + i) * d_vlen],
                                        d_vlen);
                }
            }
        }

        if (d_det == AVG) {
            // divide by d_meas_interval = multiply by 1/d_meas_interval
            const float scalar = 1 / static_cast<float>(d_meas_interval);
            volk_32f_s32f_multiply_32f(out, out, scalar, d_vlen * noutput_items);
        }
    }

    // Tell runtime system how many output items we produced.
    return noutput_items;
}

} /* namespace msod_sensor */
} /* namespace gr */
