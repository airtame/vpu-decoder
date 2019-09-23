/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

extern "C" {
#include <vpu_io.h>
#include <vpu_lib.h>
}

#include <memory>

namespace airtame {
// shared pointer "deleter", that is function that ensures proper release
// of dma memory when last reference is removed
void dma_pointer_deleter(vpu_mem_desc *dma);
// shared pointer type
typedef std::shared_ptr<vpu_mem_desc> VPUDMAPointer;
} // namespace airtame
