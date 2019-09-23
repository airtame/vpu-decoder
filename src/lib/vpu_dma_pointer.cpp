/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include "vpu_dma_pointer.hpp"

namespace airtame {
void dma_pointer_deleter(vpu_mem_desc *dma)
{
    if (dma->virt_uaddr) {
        /* Gotta unmap */
        IOFreeVirtMem(dma);
    }
    if (dma->phy_addr) {
        /* Gotta release */
        IOFreePhyMem(dma);
    }
    delete dma;
}
} // namespace airtame
