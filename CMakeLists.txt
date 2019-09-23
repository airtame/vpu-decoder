cmake_minimum_required(VERSION 2.8)

project(vpu-decoder)

set (TARGET_NAME vpu-decoder)

set (SOURCES
  src/lib/codec_common.hpp
  src/lib/codec_logger.hpp
  src/lib/h264_bitstream.cpp
  src/lib/h264_bitstream.hpp
  src/lib/h264_nal.hpp
  src/lib/h264_nal.cpp
  src/lib/h264_stream_parser.hpp
  src/lib/h264_stream_parser.cpp
  src/lib/ivf.h
  src/lib/jpeg_parser.hpp
  src/lib/jpeg_parser.cpp
  src/lib/timestamp.hpp
  src/lib/vp8_stream_parser.hpp
  src/lib/vp8_stream_parser.cpp
  src/lib/vpu.hpp
  src/lib/vpu.cpp
  src/lib/vpu_bitstream_buffer_monitoring.cpp
  src/lib/vpu_bitstream_buffer_monitoring.hpp
  src/lib/vpu_decoder_buffers.hpp
  src/lib/vpu_decoder.cpp
  src/lib/vpu_decoder.hpp
  src/lib/vpu_decoding_session.cpp
  src/lib/vpu_decoding_session.hpp
  src/lib/vpu_dma_pointer.cpp
  src/lib/vpu_dma_pointer.hpp
  src/lib/vpu_h264_decoder.hpp
  src/lib/vpu_h264_decoder.cpp
  src/lib/vpu_jpeg_decoder.hpp
  src/lib/vpu_jpeg_decoder.cpp
  src/lib/vpu_vp8_decoder.hpp
  src/lib/vpu_vp8_decoder.cpp
)

add_library (${TARGET_NAME} STATIC ${SOURCES})

project(vpu_playback)

set (TARGET_NAME vpu_playback)

include_directories (${CMAKE_CURRENT_SOURCE_DIR}/src/lib)

set (SOURCES
  src/player/g2d_display.cpp
  src/player/g2d_display.hpp
  src/player/stream.cpp
  src/player/stream.hpp
  src/player/stream_handler.hpp
  src/player/h264_stream_handler.hpp
  src/player/h264_stream_handler.cpp
  src/player/jpeg_stream_handler.hpp
  src/player/jpeg_stream_handler.cpp
  src/player/vp8_stream_handler.hpp
  src/player/vp8_stream_handler.cpp
  src/player/main.cpp
)

set (LIBS vpu-decoder vpu g2d)

add_executable (${TARGET_NAME} ${SOURCES})
target_link_libraries (${TARGET_NAME} ${LIBS})
