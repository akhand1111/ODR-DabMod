/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010 Her Majesty the
   Queen in Right of Canada (Communications Research Center Canada)

   Copyright (C) 2017
   Matthias P. Braendli, matthias.braendli@mpb.li

    http://opendigitalradio.org
 */
/*
   This file is part of ODR-DabMod.

   ODR-DabMod is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   ODR-DabMod is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with ODR-DabMod.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <queue>
#include <iostream>
#include <fstream>
#include <string>
#include <sys/types.h>
#include "PcDebug.h"
#include "TimestampDecoder.h"
#include "Eti.h"
#include "Log.h"

//#define MDEBUG(fmt, args...) fprintf (LOG, "*****" fmt , ## args)
#define MDEBUG(fmt, args...) PDEBUG(fmt, ## args)

TimestampDecoder::TimestampDecoder(double& offset_s) :
        RemoteControllable("tist"),
        timestamp_offset(offset_s)
{
    // Properly initialise temp_time
    memset(&temp_time, 0, sizeof(temp_time));
    const time_t timep = 0;
    gmtime_r(&timep, &temp_time);

    RC_ADD_PARAMETER(offset, "TIST offset [s]");
    RC_ADD_PARAMETER(timestamp, "FCT and timestamp [s]");

    etiLog.level(info) << "Setting up timestamp decoder with " <<
        timestamp_offset << " offset";
}

std::shared_ptr<frame_timestamp> TimestampDecoder::getTimestamp()
{
    std::shared_ptr<frame_timestamp> ts =
        std::make_shared<frame_timestamp>();

    /* Push new timestamp into queue */
    ts->timestamp_valid = full_timestamp_received;
    ts->timestamp_sec = time_secs;
    ts->timestamp_pps = time_pps;
    ts->fct = latestFCT;
    ts->fp = latestFP;

    ts->timestamp_refresh = offset_changed;
    offset_changed = false;

    MDEBUG("time_secs=%d, time_pps=%f\n", time_secs,
            (double)time_pps / 16384000.0);
    *ts += timestamp_offset;

    return ts;
}

void TimestampDecoder::pushMNSCData(uint8_t framephase, uint16_t mnsc)
{
    struct eti_MNSC_TIME_0 *mnsc0;
    struct eti_MNSC_TIME_1 *mnsc1;
    struct eti_MNSC_TIME_2 *mnsc2;
    struct eti_MNSC_TIME_3 *mnsc3;

    switch (framephase)
    {
        case 0:
            mnsc0 = (struct eti_MNSC_TIME_0*)&mnsc;
            enableDecode = (mnsc0->type == 0) &&
                (mnsc0->identifier == 0);
            {
                const time_t timep = 0;
                gmtime_r(&timep, &temp_time);
            }
            break;

        case 1:
            mnsc1 = (struct eti_MNSC_TIME_1*)&mnsc;
            temp_time.tm_sec = mnsc1->second_tens * 10 + mnsc1->second_unit;
            temp_time.tm_min = mnsc1->minute_tens * 10 + mnsc1->minute_unit;

            if (!mnsc1->sync_to_frame)
            {
                enableDecode = false;
                PDEBUG("TimestampDecoder: MNSC time info is not synchronised to frame\n");
            }

            break;

        case 2:
            mnsc2 = (struct eti_MNSC_TIME_2*)&mnsc;
            temp_time.tm_hour = mnsc2->hour_tens * 10 + mnsc2->hour_unit;
            temp_time.tm_mday = mnsc2->day_tens * 10 + mnsc2->day_unit;
            break;

        case 3:
            mnsc3 = (struct eti_MNSC_TIME_3*)&mnsc;
            temp_time.tm_mon = (mnsc3->month_tens * 10 + mnsc3->month_unit) - 1;
            temp_time.tm_year = (mnsc3->year_tens * 10 + mnsc3->year_unit) + 100;

            if (enableDecode)
            {
                full_timestamp_received = true;
                updateTimestampSeconds(mktime(&temp_time));
            }
            break;
    }

    MDEBUG("TimestampDecoder::pushMNSCData(%d, 0x%x)\n", framephase, mnsc);
    MDEBUG("                            -> %s\n", asctime(&temp_time));
    MDEBUG("                            -> %zu\n", mktime(&temp_time));
}

void TimestampDecoder::updateTimestampSeconds(uint32_t secs)
{
    if (inhibit_second_update > 0)
    {
        MDEBUG("TimestampDecoder::updateTimestampSeconds(%d) inhibit\n", secs);
        inhibit_second_update--;
    }
    else
    {
        MDEBUG("TimestampDecoder::updateTimestampSeconds(%d) apply\n", secs);
        time_secs = secs;
    }
}

void TimestampDecoder::updateTimestampPPS(uint32_t pps)
{
    MDEBUG("TimestampDecoder::updateTimestampPPS(%f)\n", (double)pps / 16384000.0);

    if (time_pps > pps) // Second boundary crossed
    {
        MDEBUG("TimestampDecoder::updateTimestampPPS crossed second\n");

        // The second for the next eight frames will not
        // be defined by the MNSC
        inhibit_second_update = 2;
        time_secs += 1;
    }

    time_pps = pps;
}

void TimestampDecoder::updateTimestampEti(
        uint8_t framephase,
        uint16_t mnsc,
        uint32_t pps, // In units of 1/16384000 s
        int32_t fct)
{
    updateTimestampPPS(pps);
    pushMNSCData(framephase, mnsc);
    latestFCT = fct;
    latestFP = framephase;
}

void TimestampDecoder::updateTimestampEdi(
        uint32_t seconds_utc,
        uint32_t pps, // In units of 1/16384000 s
        int32_t fct,
        uint8_t framephase)
{
    time_secs = seconds_utc;
    time_pps  = pps;
    latestFCT = fct;
    latestFP = framephase;
    full_timestamp_received = true;
}

void TimestampDecoder::set_parameter(
        const std::string& parameter,
        const std::string& value)
{
    using namespace std;

    stringstream ss(value);
    ss.exceptions ( stringstream::failbit | stringstream::badbit );

    if (parameter == "offset") {
        ss >> timestamp_offset;
        offset_changed = true;
    }
    else if (parameter == "timestamp") {
        throw ParameterError("timestamp is read-only");
    }
    else {
        stringstream ss_err;
        ss_err << "Parameter '" << parameter
            << "' is not exported by controllable " << get_rc_name();
        throw ParameterError(ss_err.str());
    }
}

const std::string TimestampDecoder::get_parameter(
        const std::string& parameter) const
{
    using namespace std;

    stringstream ss;
    if (parameter == "offset") {
        ss << timestamp_offset;
    }
    else if (parameter == "timestamp") {
        if (full_timestamp_received) {
            ss.setf(std::ios_base::fixed, std::ios_base::floatfield);
            ss << time_secs + ((double)time_pps / 16384000.0) <<
                " for frame FCT " << latestFCT;
        }
        else {
            throw ParameterError("Not available yet");
        }
    }
    else {
        ss << "Parameter '" << parameter <<
            "' is not exported by controllable " << get_rc_name();
        throw ParameterError(ss.str());
    }
    return ss.str();
}

