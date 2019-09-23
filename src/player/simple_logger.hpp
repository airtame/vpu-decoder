/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <stdarg.h>
#include <stdio.h>

#include "codec_logger.hpp"

namespace airtame {
class SimpleLogger : public CodecLogger {
public:
    void Log(int severity, const char *file, const char *func, int line,
             const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        fprintf(stderr, "Decoder %s (%s in %s, line %d): ",
                level_to_string(severity), func, file, line);
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

private:
    static const char *level_to_string(int severity)
    {
        switch (severity) {
            case CodecLogger::TRACE:
                return "trace";
            case CodecLogger::DEBUG:
                return "debug";
            case CodecLogger::ERROR:
                return "error";
            case CodecLogger::FATAL:
                return "fatal";
            case CodecLogger::WARNING:
                return "warning";
            case CodecLogger::INFO:
            default:
                return "info";
        }
    }
};
}
