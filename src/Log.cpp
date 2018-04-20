/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012
   Her Majesty the Queen in Right of Canada (Communications Research
   Center Canada)

   Copyright (C) 2016
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

#include <list>
#include <stdarg.h>
#include <chrono>

#include "Log.h"
#include "Utils.h"

using namespace std;

/* This is called etiLog because it was copy-pasted from ODR-DabMux, even
 * though it doesn't make any more sense there than here.
 *
 * It is a singleton used in all parts of ODR-DabMod to output log messages.
 */
Logger etiLog;


void Logger::register_backend(LogBackend* backend) {
    backends.push_back(backend);
    //log(info, "Registered new logger " + backend->get_name());
}


void Logger::log(log_level_t level, const char* fmt, ...)
{
    int size = 100;
    std::string str;
    va_list ap;
    while (1) {
        str.resize(size);
        va_start(ap, fmt);
        int n = vsnprintf((char *)str.c_str(), size, fmt, ap);
        va_end(ap);
        if (n > -1 && n < size) {
            str.resize(n);
            break;
        }
        if (n > -1)
            size = n + 1;
        else
            size *= 2;
    }

    logstr(level, str);
}

void Logger::logstr(log_level_t level, std::string message)
{
    log_message_t m(level, message);
    m_message_queue.push(std::move(m));
}

void Logger::io_process()
{
    set_thread_name("logger");
    while (1) {
        log_message_t m;
        try {
            m_message_queue.wait_and_pop(m);
        }
        catch (const ThreadsafeQueueWakeup&) {
            break;
        }

        auto message = m.message;

        /* Remove a potential trailing newline.
         * It doesn't look good in syslog
         */
        if (message[message.length()-1] == '\n') {
            message.resize(message.length()-1);
        }

        for (auto &backend : backends) {
            backend->log(m.level, message);
        }

        if (m.level != log_level_t::trace) {
            std::lock_guard<std::mutex> guard(m_cerr_mutex);
            std::cerr << levels_as_str[m.level] << " " << message << std::endl;
        }
    }
}

LogLine Logger::level(log_level_t level)
{
    return LogLine(this, level);
}

LogToFile::LogToFile(const std::string& filename) : name("FILE")
{
    FILE* fd = fopen(filename.c_str(), "a");
    if (fd == nullptr) {
        fprintf(stderr, "Cannot open log file !");
        throw std::runtime_error("Cannot open log file !");
    }

    log_file.reset(fd);
}

void LogToFile::log(log_level_t level, const std::string& message)
{
    if (level != log_level_t::trace) {
        const char* log_level_text[] = {
            "DEBUG", "INFO", "WARN", "ERROR", "ALERT", "EMERG"};

        // fprintf is thread-safe
        fprintf(log_file.get(), SYSLOG_IDENT ": %s: %s\n",
                log_level_text[(size_t)level], message.c_str());
        fflush(log_file.get());
    }
}

void LogToSyslog::log(log_level_t level, const std::string& message)
{
    if (level != log_level_t::trace) {
        int syslog_level = LOG_EMERG;
        switch (level) {
            case trace: break; // Do not handle TRACE in syslog
            case debug: syslog_level = LOG_DEBUG; break;
            case info:  syslog_level = LOG_INFO; break;
                        /* we don't have the notice level */
            case warn:  syslog_level = LOG_WARNING; break;
            case error: syslog_level = LOG_ERR; break;
            default:    syslog_level = LOG_CRIT; break;
            case alert: syslog_level = LOG_ALERT; break;
            case emerg: syslog_level = LOG_EMERG; break;
        }

        syslog(syslog_level, SYSLOG_IDENT " %s", message.c_str());
    }
}

LogTracer::LogTracer(const string& trace_filename) : name("TRACE")
{
    etiLog.level(info) << "Setting up TRACE to " << trace_filename;

    FILE* fd = fopen(trace_filename.c_str(), "a");
    if (fd == nullptr) {
        fprintf(stderr, "Cannot open trace file !");
        throw std::runtime_error("Cannot open trace file !");
    }
    m_trace_file.reset(fd);

    using namespace std::chrono;
    auto now = steady_clock::now().time_since_epoch();
    m_trace_micros_startup = duration_cast<microseconds>(now).count();

    fprintf(m_trace_file.get(),
            "0,TRACER,startup at %ld\n", m_trace_micros_startup);
}

void LogTracer::log(log_level_t level, const std::string& message)
{
    if (level == log_level_t::trace) {
        using namespace std::chrono;
        const auto now = steady_clock::now().time_since_epoch();
        const auto micros = duration_cast<microseconds>(now).count();

        fprintf(m_trace_file.get(), "%ld,%s\n",
                micros - m_trace_micros_startup,
                message.c_str());
    }
}
