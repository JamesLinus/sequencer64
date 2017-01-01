/*
 *  This file is part of seq24/sequencer64.
 *
 *  seq24 is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  seq24 is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with seq24; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * \file          mastermidibus.cpp
 *
 *  This module declares/defines the base class for handling MIDI I/O via
 *  the ALSA system.
 *
 * \library       sequencer64 application
 * \author        Seq24 team; modifications by Chris Ahlstrom
 * \date          2015-07-30
 * \updates       2016-12-31
 * \license       GNU GPLv2 or above
 *
 *  This file provides a Linux-only implementation of ALSA MIDI support.
 *
 *  Manual ALSA Ports:
 *
 *      This option has the following features when creating new midibus
 *      objects in api_init():
 *
 *      -   The short midibus constructor is called.
 *      -   For each input buss, midibus::init_in_sub() is called.
 *      -   For each output buss, midibus::init_out_sub() is called.
 *
 *  Regular ALSA Ports:
 *
 *      This option has the following features when creating new midibus
 *      objects in api_init():
 *
 *      -   The long midibus constructor is called.
 *      -   For each input buss, midibus::init_in() is NOT (!) called.
 *          This function is called in midibase::set_input() if the inputing
 *          parameter is true, though.  It is also called in the PortMidi
 *          version of mastermidibus::init().
 *      -   For each output buss, midibus::init_out() is called.
 *          This function is also called in the api_port_start() function!
 */

#include "easy_macros.h"

#ifdef SEQ64_HAVE_LIBASOUND
#include <sys/poll.h>
#ifdef SEQ64_LASH_SUPPORT
#include "lash.hpp"
#endif
#endif

#include "calculations.hpp"             /* tempo_from_beats_per_minute()    */
#include "event.hpp"                    /* seq64::event                     */
#include "mastermidibus.hpp"            /* seq64::mastermidibus             */
#include "midibus.hpp"                  /* seq64::midibus for ALSA          */
#include "sequence.hpp"                 /* seq64::sequence                  */
#include "settings.hpp"                 /* seq64::rc() and choose_ppqn()    */

/**
 *  Macros to make capabilities-checking more readable.
 */

#define CAP_READ(cap)       (((cap) & SND_SEQ_PORT_CAP_SUBS_READ) != 0)
#define CAP_WRITE(cap)      (((cap) & SND_SEQ_PORT_CAP_SUBS_WRITE) != 0)

/**
 *  These checks need both bits to be set.  Intermediate macros used for
 *  readability.
 */

#define CAP_R_BITS      (SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_READ)
#define CAP_W_BITS      (SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_WRITE)

#define CAP_FULL_READ(cap)  (((cap) & CAP_R_BITS) == CAP_R_BITS)
#define CAP_FULL_WRITE(cap) (((cap) & CAP_W_BITS) == CAP_W_BITS)

#define ALSA_CLIENT_CHECK(pinfo) \
    (snd_seq_client_id(m_alsa_seq) != snd_seq_port_info_get_client(pinfo))

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq64
{

/**
 *  The mastermidibus default constructor fills the array with our busses.
 *
 * \param ppqn
 *      Provides the PPQN value for this object.  However, in most cases, the
 *      default, SEQ64_USE_DEFAULT_PPQN should be specified.  Then the caller
 *      of this constructor should call mastermidibus::api_set_ppqn() to set up
 *      the proper PPQN value.
 *
 * \param bpm
 *      Provides the beats per minute value, which defaults to
 *      c_beats_per_minute.  Must be handled similarly to ppqn.
 */

mastermidibus::mastermidibus (int ppqn, int bpm)
 :
    mastermidibase          (ppqn, bpm),
    m_alsa_seq              (nullptr),
    m_num_poll_descriptors  (0),
    m_poll_descriptors      (nullptr)
{
    /*
     * Open the sequencer client.  This line of code results in a loss of
     * 4 bytes somewhere in snd_seq_open(), as discovered via valgrind.
     */

    int result = snd_seq_open(&m_alsa_seq, "default",  SND_SEQ_OPEN_DUPLEX, 0);
    if (result < 0)
    {
        errprint("snd_seq_open() error");
        exit(1);
    }
    else
    {
        /*
         * Tried to reduce apparent memory leaks from libasound, but this call
         * changed nothing.
         *
         * (void) snd_config_update_free_global();
         */
    }

    /*
     * Set the client's name for ALSA.  It used to be "seq24".  Then set up our
     * ALSA client's queue.
     */

    snd_seq_set_client_name(m_alsa_seq, SEQ64_PACKAGE); /* "sequencer64"    */
    m_queue = snd_seq_alloc_queue(m_alsa_seq);          /* protected member */

#ifdef SEQ64_LASH_SUPPORT

    /*
     * Notify LASH of our client ID so that it can restore connections.
     */

    if (not_nullptr(lash_driver()))
        lash_driver()->set_alsa_client_id(snd_seq_client_id(m_alsa_seq));

#endif
}

/**
 *  The destructor deletes all of the output busses, clears out the ALSA
 *  events, stops and frees the queue, and closes ALSA for this
 *  application.
 *
 *  Valgrind indicates we might have issues caused by the following functions:
 *
 *      -   snd_config_hook_load()
 *      -   snd_config_update_r() via snd_seq_open()
 *      -   _dl_init() and other GNU function
 *      -   init_gtkmm_internals() [version 2.4]
 */

mastermidibus::~mastermidibus ()
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);                          /* memsets it to 0      */
    snd_seq_stop_queue(m_alsa_seq, m_queue, &ev);
    snd_seq_free_queue(m_alsa_seq, m_queue);
    snd_seq_close(m_alsa_seq);                      /* close client         */
    (void) snd_config_update_free_global();         /* additional cleanup   */

    /*
     * Still more cleanup, not in seq24.
     */

    if (not_nullptr(m_poll_descriptors))
    {
        delete [] m_poll_descriptors;
        m_poll_descriptors = nullptr;
    }
}

/**
 *  Initialize the mastermidibus.  It initializes 16 MIDI output busses, a
 *  hardwired constant, SEQ64_ALSA_OUTPUT_BUSS_MAX == 16.  Only one MIDI input
 *  buss is initialized.
 *
 * \note
 *      We now start the buss numbers at 0 in manual mode, so they match the
 *      number base (0) in normal mode, where the system is queried for the
 *      ports.
 *
 * \todo
 *      We still need to reset the PPQN and BPM values via the ALSA API
 *      if they are different here!  See the "rtmidi" implementation of
 *      this function.
 *
 * \param ppqn
 *      The PPQN value to which to initialize the master MIDI buss.
 *
 * \param bpm
 *      The BPM value to which to initialize the master MIDI buss, if
 *      applicable.
 */

void
mastermidibus::api_init (int ppqn, int bpm)
{
    if (rc().manual_alsa_ports())
    {
        int num_buses = SEQ64_ALSA_OUTPUT_BUSS_MAX;
        for (int i = 0; i < num_buses; ++i)             /* output busses    */
        {
#ifdef USE_BUS_ARRAY_CODE
            midibus * m = new midibus                   /* virtual port     */
            (
                snd_seq_client_id(m_alsa_seq), m_alsa_seq, i, m_queue, ppqn, bpm
            );
            m_outbus_array.add(m, false, true);            /* output & virtual */
#else
            if (not_nullptr(m_buses_out[i]))
            {
                delete m_buses_out[i];
                errprintf("manual: m_buses_out[%d] not null\n", i);
            }
            m_buses_out[i] = new midibus                /* virtual port     */
            (
                snd_seq_client_id(m_alsa_seq), m_alsa_seq, i, m_queue, ppqn, bpm
            );
            m_buses_out[i]->init_out_sub();
            m_buses_out_active[i] = m_buses_out_init[i] = true;
#endif
        }
#ifndef USE_BUS_ARRAY_CODE
        m_num_out_buses = num_buses;
#endif

#ifdef USE_BUS_ARRAY_CODE
        midibus * m = new midibus                       /* virtual port     */
        (
            snd_seq_client_id(m_alsa_seq), m_alsa_seq, 0, m_queue, ppqn, bpm
        );
        m_inbus_array.add(m, true, true);                 /* input & virtual  */
#else
        if (not_nullptr(m_buses_in[0]))
        {
            delete m_buses_in[0];
            errprint("manual: m_buses_[0] not null");
        }

        /*
         * Input buss.  Only the first element is set up.  The rest are used
         * only for non-manual ALSA ports in the else-class below.
         */

        m_num_in_buses = 1;
        m_buses_in[0] = new midibus                     /* virtual port     */
        (
            snd_seq_client_id(m_alsa_seq), m_alsa_seq, 0, m_queue, ppqn, bpm
        );
        m_buses_in[0]->init_in_sub();
        m_buses_in_active[0] = m_buses_in_init[0] = true;
#endif
    }
    else
    {
        /*
         * While the next client for the sequencer is available, get the client
         * from cinfo.  Fill pinfo.
         */

        snd_seq_client_info_t * cinfo;                      /* client info      */
        snd_seq_port_info_t * pinfo;                        /* port info        */
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_client_info_set_client(cinfo, -1);
#ifdef USE_BUS_ARRAY_CODE
        int numouts = 0;
        int numins = 0;
#endif
        while (snd_seq_query_next_client(m_alsa_seq, cinfo) >= 0)
        {
            int client = snd_seq_client_info_get_client(cinfo);
            snd_seq_port_info_alloca(&pinfo);           /* will fill pinfo */
            snd_seq_port_info_set_client(pinfo, client);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(m_alsa_seq, pinfo) >= 0)
            {
                /*
                 * While the next port is available, get its capability.
                 */

                int cap = snd_seq_port_info_get_capability(pinfo);
                if
                (
                    ALSA_CLIENT_CHECK(pinfo) &&
                    snd_seq_port_info_get_client(pinfo) != SND_SEQ_CLIENT_SYSTEM
                )
                {
                    /*
                     * Output busses.  Why do the ALSA client check again
                     * here?  Because it could be altered in the if-clause
                     * above.
                     */

                    if (CAP_WRITE(cap) && ALSA_CLIENT_CHECK(pinfo))
                    {
#ifdef USE_BUS_ARRAY_CODE
                        midibus * m = new midibus
                        (
                            snd_seq_client_id(m_alsa_seq),
                            snd_seq_port_info_get_client(pinfo),
                            snd_seq_port_info_get_port(pinfo),
                            m_alsa_seq,
                            snd_seq_client_info_get_name(cinfo),
                            snd_seq_port_info_get_name(pinfo),
                            numouts, m_queue, ppqn, bpm
                        );
                        m_outbus_array.add(m, false, false);   /* output, nonvirt */
                        ++numouts;
#else
                        if (not_nullptr(m_buses_out[m_num_out_buses]))
                        {
                            delete m_buses_out[m_num_out_buses];
                            errprintf
                            (
                                "mmbus::init(): m_buses_out[%d] not null\n",
                                m_num_out_buses
                            );
                        }
                        m_buses_out[m_num_out_buses] = new midibus
                        (
                            snd_seq_client_id(m_alsa_seq),
                            snd_seq_port_info_get_client(pinfo),
                            snd_seq_port_info_get_port(pinfo),
                            m_alsa_seq,
                            snd_seq_client_info_get_name(cinfo),
                            snd_seq_port_info_get_name(pinfo),
                            m_num_out_buses, m_queue, ppqn, bpm
                        );
                        if (m_buses_out[m_num_out_buses]->init_out())
                        {
                            m_buses_out_active[m_num_out_buses] = true;
                            m_buses_out_init[m_num_out_buses] = true;
                        }
                        else
                            m_buses_out_init[m_num_out_buses] = true;

                        ++m_num_out_buses;
#endif
                    }

                    /*
                     * Input busses
                     */

                    if (CAP_READ(cap) && ALSA_CLIENT_CHECK(pinfo)) /* inputs */
                    {
#ifdef USE_BUS_ARRAY_CODE
                        midibus * m = new midibus
                        (
                            snd_seq_client_id(m_alsa_seq),
                            snd_seq_port_info_get_client(pinfo),
                            snd_seq_port_info_get_port(pinfo),
                            m_alsa_seq,
                            snd_seq_client_info_get_name(cinfo),
                            snd_seq_port_info_get_name(pinfo),
                            numins, m_queue, ppqn, bpm
                        );
                        m_inbus_array.add(m, true, false);   /* input, nonvirt */
                        ++numins;
#else
                        if (not_nullptr(m_buses_in[m_num_in_buses]))
                        {
                            delete m_buses_in[m_num_in_buses];
                            errprintf
                            (
                                "mmbus::init(): m_buses_in[%d] not null\n",
                                m_num_in_buses
                            );
                        }
                        m_buses_in[m_num_in_buses] = new midibus
                        (
                            snd_seq_client_id(m_alsa_seq),
                            snd_seq_port_info_get_client(pinfo),
                            snd_seq_port_info_get_port(pinfo),
                            m_alsa_seq,
                            snd_seq_client_info_get_name(cinfo),
                            snd_seq_port_info_get_name(pinfo),
                            m_num_in_buses, m_queue, ppqn, bpm
                        );

                        /*
                         * Why no call to midibus::init_in()?  Commented out in
                         * seq24!
                         */

                        m_buses_in_active[m_num_in_buses] =
                            m_buses_in_init[m_num_in_buses] = true;

                        ++m_num_in_buses;
#endif
                    }
                }
            }
        }                                       /* end loop for clients */
    }
    set_beats_per_minute(m_beats_per_minute);
    set_ppqn(ppqn);

    // set_sequence_input(false, NULL);

    /*
     * Get the number of MIDI input poll file descriptors.  Allocate the
     * poll-descriptors array.  Then get the input poll-descriptors into the
     * array.  Then set the input and output buffer sizes.   Then create an
     * announcment buss.
     */

    m_num_poll_descriptors = snd_seq_poll_descriptors_count(m_alsa_seq, POLLIN);
    m_poll_descriptors = new pollfd[m_num_poll_descriptors];
    snd_seq_poll_descriptors
    (
        m_alsa_seq, m_poll_descriptors, m_num_poll_descriptors, POLLIN
    );
    set_sequence_input(false, nullptr);
    snd_seq_set_output_buffer_size(m_alsa_seq, c_midibus_output_size);
    snd_seq_set_input_buffer_size(m_alsa_seq, c_midibus_input_size);
    m_bus_announce = new midibus
    (
        snd_seq_client_id(m_alsa_seq),
        SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE,
        m_alsa_seq, "system", "announce",   // was "annouce" ca 2016-04-03
        0, m_queue, ppqn, bpm
    );
    m_bus_announce->set_input(true);

#ifdef USE_BUS_ARRAY_CODE
    m_outbus_array.set_all_clocks();
    m_inbus_array.set_all_inputs();
#else
    for (int i = 0; i < m_num_out_buses; ++i)
        set_clock(i, m_init_clock[i]);

    for (int i = 0; i < m_num_in_buses; ++i)
        set_input(i, m_init_input[i]);
#endif
}

/**
 *  Starts all of the configured output busses up to m_num_out_buses.
 *
 * \threadsafe
 */

void
mastermidibus::api_start ()
{
    snd_seq_start_queue(m_alsa_seq, m_queue, NULL);     /* start timer */
}

/**
 *  Gets the output busses running again, if ALSA support is enabled.
 *
 * \threadsafe
 *
 * \param tick
 *      Provides the tick value to continue from.  Not used in the ALSA
 *      implementation.
 */

void
mastermidibus::api_continue_from (midipulse /* tick */)
{
    snd_seq_start_queue(m_alsa_seq, m_queue, NULL);     /* start timer */
}

/**
 *  Stops each of the output busses.  If ALSA support is enable, also drains
 *  the output, synchronizes the output queue, and then stop the queue.
 *
 * \threadsafe
 */

void
mastermidibus::api_stop ()
{
    snd_seq_drain_output(m_alsa_seq);
    snd_seq_sync_output_queue(m_alsa_seq);
    snd_seq_stop_queue(m_alsa_seq, m_queue, NULL);  /* start timer */
}

/**
 *  Set the PPQN value (parts per quarter note).  This is done by creating an
 *  ALSA tempo structure, adding tempo information to it, and then setting the
 *  ALSA sequencer object with this information.  Fills the tempo structure
 *  with the current tempo information.  Then sets the ppqn value.  Finally,
 *  gives the tempo structure to the ALSA queue.
 *
 * \threadsafe
 *
 * \param ppqn
 *      The PPQN value to be set.
 */

void
mastermidibus::api_set_ppqn (int p)
{
    snd_seq_queue_tempo_t * tempo;
    snd_seq_queue_tempo_alloca(&tempo);             /* allocate tempo struct */
    snd_seq_get_queue_tempo(m_alsa_seq, m_queue, tempo);
    snd_seq_queue_tempo_set_ppq(tempo, p);
    snd_seq_set_queue_tempo(m_alsa_seq, m_queue, tempo);
}

/**
 *  Set the BPM value (beats per minute).  This is done by creating
 *  an ALSA tempo structure, adding tempo information to it, and then
 *  setting the ALSA sequencer object with this information.
 *
 *  We fill the ALSA tempo structure (snd_seq_queue_tempo_t) with the current
 *  tempo information, set the BPM value, put it in the tempo structure, and
 *  give the tempo value to the ALSA queue.
 *
 * \threadsafe
 *
 * \param bpm
 *      Provides the beats-per-minute value to set.
 */

void
mastermidibus::api_set_beats_per_minute (int b)
{
    snd_seq_queue_tempo_t * tempo;
    snd_seq_queue_tempo_alloca(&tempo);          /* allocate tempo struct */
    snd_seq_get_queue_tempo(m_alsa_seq, m_queue, tempo);
    snd_seq_queue_tempo_set_tempo
    (
        tempo, int(tempo_us_from_beats_per_minute(b))
    );
    snd_seq_set_queue_tempo(m_alsa_seq, m_queue, tempo);
}

/**
 *  Flushes our local queue events out into ALSA.
 *
 * \threadsafe
 */

void
mastermidibus::api_flush ()
{
    snd_seq_drain_output(m_alsa_seq);
}

/**
 *  Initiate a poll() on the existing poll descriptors.
 *
 *  No locking needed?
 *
 * \return
 *      Returns the result of the poll, or 0 if ALSA is not supported.
 */

int
mastermidibus::api_poll_for_midi ()
{
    return poll(m_poll_descriptors, m_num_poll_descriptors, 1000);
}

/**
 *  Test the ALSA sequencer to see if any more input is pending.
 *
 * \threadsafe
 *
 * \return
 *      Returns true if ALSA is supported, and the returned size is greater
 *      than 0, or false otherwise.
 */

bool
mastermidibus::api_is_more_input ()
{
    automutex locker(m_mutex);
    return snd_seq_event_input_pending(m_alsa_seq, 0) > 0;
}

/**
 *  Start the given ALSA MIDI port.
 *
 *  \threadsafe
 *      Quite a lot is done during the lock!
 *
 * \param bus
 *      Provides the ALSA client number.
 *
 * \param port
 *      Provides the ALSA client port.
 */

void
mastermidibus::api_port_start (int bus, int port)
{
    snd_seq_client_info_t * cinfo;                      /* client info        */
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_get_any_client_info(m_alsa_seq, bus, cinfo);
    snd_seq_port_info_t * pinfo;                        /* port info          */
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_get_any_port_info(m_alsa_seq, bus, port, pinfo);

    int cap = snd_seq_port_info_get_capability(pinfo);  /* get its capability */
    if (ALSA_CLIENT_CHECK(pinfo))
    {
        if (CAP_FULL_WRITE(cap) && ALSA_CLIENT_CHECK(pinfo)) /* outputs */
        {
#ifdef USE_BUS_ARRAY_CODE
//          bool replacement = false;
            int bus_slot = m_outbus_array.count();
            int test = m_outbus_array.replacement_port(bus, port);
            if (test >= 0)
            {
//              replacement = true;
                bus_slot = test;
            }
            midibus * m = new midibus
            (
                snd_seq_client_id(m_alsa_seq),
                snd_seq_port_info_get_client(pinfo),
                snd_seq_port_info_get_port(pinfo),
                m_alsa_seq,
                snd_seq_client_info_get_name(cinfo),
                snd_seq_port_info_get_name(pinfo),
                bus_slot,
                m_queue, get_ppqn(), get_bpm()
            );
            m_outbus_array.add(m, false, false);  /* out, nonvirt */
#else
            bool replacement = false;
            int bus_slot = m_num_out_buses;
            for (int i = 0; i < m_num_out_buses; ++i)
            {
                if (m_buses_out[i]->match(bus, port) && ! m_buses_out_active[i])
                {
                    replacement = true;
                    bus_slot = i;
                }
            }
            if (not_nullptr(m_buses_out[bus_slot]))
            {
                delete m_buses_out[bus_slot];
                errprintf("port_start(): m_buses_out[%d] not null\n", bus_slot);
            }
            m_buses_out[bus_slot] = new midibus
            (
                snd_seq_client_id(m_alsa_seq),
                snd_seq_port_info_get_client(pinfo),
                snd_seq_port_info_get_port(pinfo),
                m_alsa_seq,
                snd_seq_client_info_get_name(cinfo),
                snd_seq_port_info_get_name(pinfo),
                m_num_out_buses,
                m_queue, get_ppqn(), get_bpm()
            );
            m_buses_out[bus_slot]->init_out();
            m_buses_out_active[bus_slot] = m_buses_out_init[bus_slot] = true;
            if (! replacement)
                ++m_num_out_buses;
#endif  // USE_BUS_ARRAY_CODE
        }
        if (CAP_FULL_READ(cap) && ALSA_CLIENT_CHECK(pinfo)) /* inputs */
        {
#ifdef USE_BUS_ARRAY_CODE
            int bus_slot = m_inbus_array.count();
            int test = m_inbus_array.replacement_port(bus, port);
            if (test >= 0)
            {
                bus_slot = test;
            }
            midibus * m = new midibus
            (
                snd_seq_client_id(m_alsa_seq),
                snd_seq_port_info_get_client(pinfo),
                snd_seq_port_info_get_port(pinfo),
                m_alsa_seq,
                snd_seq_client_info_get_name(cinfo),
                snd_seq_port_info_get_name(pinfo),
                bus_slot,
                m_queue, get_ppqn(), get_bpm()
            );
            m_inbus_array.add(m, false, false);  /* out, nonvirt */
#else
            bool replacement = false;
            int bus_slot = m_num_in_buses;
            for (int i = 0; i < m_num_in_buses; ++i)
            {
                if (m_buses_in[i]->match(bus, port) && ! m_buses_in_active[i])
                {
                    replacement = true;
                    bus_slot = i;
                }
            }
            if (not_nullptr(m_buses_in[bus_slot]))
            {
                delete m_buses_in[bus_slot];
                errprintf("port_start(): m_buses_in[%d] not null\n", bus_slot);
            }
            m_buses_in[bus_slot] = new midibus
            (
                snd_seq_client_id(m_alsa_seq),
                snd_seq_port_info_get_client(pinfo),
                snd_seq_port_info_get_port(pinfo),
                m_alsa_seq,
                snd_seq_client_info_get_name(cinfo),
                snd_seq_port_info_get_name(pinfo),
                m_num_in_buses,
                m_queue, get_ppqn(), get_bpm()
            );

            /*
             * Commented out in seq24:  bus_in[bus_slot]->init_in();
             */

            m_buses_in_active[bus_slot] = m_buses_in_init[bus_slot] = true;
            if (! replacement)
                ++m_num_in_buses;
#endif  // USE_BUS_ARRAY_CODE
        }
    }                                           /* end loop for clients */

    /*
     * Get the number of MIDI input poll file descriptors.
     */

    m_num_poll_descriptors = snd_seq_poll_descriptors_count(m_alsa_seq, POLLIN);
    m_poll_descriptors = new pollfd[m_num_poll_descriptors]; /* allocate info */
    snd_seq_poll_descriptors                        /* get input descriptors */
    (
        m_alsa_seq, m_poll_descriptors, m_num_poll_descriptors, POLLIN
    );
}

/**
 *  Grab a MIDI event.  First, a rather large buffer is allocated on the stack
 *  to hold the MIDI event data.  Next, if the --alsa-manual-ports option is
 *  not in force, then we check to see if the event is a port-start,
 *  port-exit, or port-change event, and we prcess it, and are done.
 *
 *  Otherwise, we create a "MIDI event parser" and decode the MIDI event.
 *
 * \threadsafe
 *
 * \param inev
 *      The event to be set based on the found input event.
 */

bool
mastermidibus::api_get_midi_event (event * inev)
{
    snd_seq_event_t * ev;
    bool sysex = false;
    bool result = false;
    midibyte buffer[0x1000];                /* temporary buffer for MIDI data */
    snd_seq_event_input(m_alsa_seq, &ev);
    if (! rc().manual_alsa_ports())
    {
        switch (ev->type)
        {
        case SND_SEQ_EVENT_PORT_START:
        {
            port_start(ev->data.addr.client, ev->data.addr.port);
            result = true;
            break;
        }
        case SND_SEQ_EVENT_PORT_EXIT:
        {
            port_exit(ev->data.addr.client, ev->data.addr.port);
            result = true;
            break;
        }
        case SND_SEQ_EVENT_PORT_CHANGE:
        {
            result = true;
            break;
        }
        default:
            break;
        }
    }
    if (result)
        return false;

    snd_midi_event_t * midi_ev;                     /* for ALSA MIDI parser  */
    snd_midi_event_new(sizeof(buffer), &midi_ev);   /* make ALSA MIDI parser */
    long bytes = snd_midi_event_decode(midi_ev, buffer, sizeof(buffer), ev);
    if (bytes <= 0)
    {
        /*
         * This happens even at startup, before anything is really happening.
         * Let's not show it.
         *
         * if (bytes < 0)
         * {
         *     errprint("error decoding MIDI event");
         * }
         */

        return false;
    }

    inev->set_timestamp(ev->time.tick);
    inev->set_status_keep_channel(buffer[0]);

    /**
     *  We will only get EVENT_SYSEX on the first packet of MIDI data;
     *  the rest we have to poll for.  SysEx processing is currently
     *  disabled.
     */

#ifdef USE_SYSEX_PROCESSING                    /* currently disabled           */
    inev->set_sysex_size(bytes);
    if (buffer[0] == EVENT_MIDI_SYSEX)
    {
        inev->restart_sysex();              /* set up for sysex if needed   */
        sysex = inev->append_sysex(buffer, bytes);
    }
    else
    {
#endif
        /*
         *  Some keyboards send Note On with velocity 0 for Note Off, so we
         *  take care of that situation here by creating a Note Off event,
         *  with the channel nybble preserved. Note that we call
         *  event :: set_status_keep_channel() instead of using stazed's
         *  set_status function with the "record" parameter.  A little more
         *  confusing, but faster.
         */

        inev->set_data(buffer[1], buffer[2]);
        if (inev->is_note_off_recorded())
            inev->set_status_keep_channel(EVENT_NOTE_OFF);

        sysex = false;

#ifdef USE_SYSEX_PROCESSING
    }
#endif

    while (sysex)       /* sysex messages might be more than one message */
    {
        snd_seq_event_input(m_alsa_seq, &ev);
        long bytes = snd_midi_event_decode(midi_ev, buffer, sizeof(buffer), ev);
        if (bytes > 0)
            sysex = inev->append_sysex(buffer, bytes);
        else
            sysex = false;
    }
    snd_midi_event_free(midi_ev);
    return true;
}

}           // namespace seq64

/*
 * mastermidibus.cpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */

