/*
 * Copyright (c) 2018  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

namespace airtame {

class CodecLogger {
public:
    enum { FATAL = 0, ERROR, WARNING, INFO, DEBUG, TRACE };

    virtual ~CodecLogger() {}

    virtual void Log(int severity, const char *file, const char *func, int line, const char *format,
                     ...)
        = 0;
};

#define codec_log_trace(logger, ...)                                                                 \
    (logger).Log(CodecLogger::TRACE, __FILE__, __func__, __LINE__, __VA_ARGS__)

#define codec_log_debug(logger, ...)                                                                 \
    (logger).Log(CodecLogger::DEBUG, __FILE__, __func__, __LINE__, __VA_ARGS__)

#define codec_log_info(logger, ...)                                                                  \
    (logger).Log(CodecLogger::INFO, __FILE__, __func__, __LINE__, __VA_ARGS__)

#define codec_log_warn(logger, ...)                                                                  \
    (logger).Log(CodecLogger::WARNING, __FILE__, __func__, __LINE__, __VA_ARGS__)

#define codec_log_error(logger, ...)                                                                 \
    (logger).Log(CodecLogger::ERROR, __FILE__, __func__, __LINE__, __VA_ARGS__)

#define codec_log_fatal(logger, ...)                                                                 \
    (logger).Log(CodecLogger::FATAL, __FILE__, __func__, __LINE__, __VA_ARGS__)

} // namespace airtame
