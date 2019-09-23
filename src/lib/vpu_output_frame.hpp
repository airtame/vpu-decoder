/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <list>

#include "codec_common.hpp"
#include "timestamp.hpp"
#include "vpu_dma_pointer.hpp"

namespace airtame {
struct VPUOutputFrame {
    VPUDMAPointer dma;
    size_t size = 0;
    Timestamp timestamp = 0;
    FrameGeometry geometry;
};

typedef std::list<VPUOutputFrame> VPUOutputFrameList;
}
