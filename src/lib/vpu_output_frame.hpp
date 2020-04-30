/*
 * Copyright (c) 2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <list>

#include "codec_common.hpp"
#include "frame_meta_data.hpp"
#include "vpu_dma_pointer.hpp"

namespace airtame {
struct VPUOutputFrame {
    VPUDMAPointer dma;
    size_t size = 0;
    std::shared_ptr<FrameMetaData> meta;
    FrameGeometry geometry;

    bool has_data()
    {
        return (bool)dma;
    }

    void reset()
    {
        dma.reset();
        meta.reset();
        size = 0;
        geometry = FrameGeometry();
    }
};

typedef std::list<VPUOutputFrame> VPUOutputFrameList;
}
