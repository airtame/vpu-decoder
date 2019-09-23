/*
 * Copyright (c) 2018-2019  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#pragma once

#include <cstdint>
#include <cinttypes>

namespace airtame {

// The VPU decoder passes timestamps from input bitstream buffers
// to output decoded frames. The VPU decoder doesn't care what the
// type is, as long as it behaves like a number type.

using Timestamp = int64_t;

}
