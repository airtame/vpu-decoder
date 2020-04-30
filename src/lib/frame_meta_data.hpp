#pragma once
/*
 * Copyright (c) 2018  AIRTAME ApS
 * All Rights Reserved.
 *
 * See LICENSE.txt for further information.
 */

#include <stddef.h>
#include <stdint.h>

#include "timestamp.hpp"

#include <memory>
#include <cassert>

namespace airtame {

class FrameMetaData {
public:
    FrameMetaData(Timestamp timestamp = 0) : m_timestamp(timestamp) {}
    virtual ~FrameMetaData() {}
    virtual void merge(const std::shared_ptr<FrameMetaData> &other)
    {
        if(other->get_timestamp() > m_timestamp) {
            m_timestamp = other->get_timestamp();
        }
    }

    Timestamp get_timestamp() const {
        return m_timestamp;
    }
    void set_timestamp(Timestamp timestamp) {
        m_timestamp = timestamp;
    }
protected:
    Timestamp m_timestamp;
};

class FrameMetaDataWithRotation : public FrameMetaData {
public:
    FrameMetaDataWithRotation(Timestamp timestamp = 0, int rotation = 0)
        : FrameMetaData(timestamp), m_rotation(rotation) {}
    virtual ~FrameMetaDataWithRotation() {}
    virtual void merge(const std::shared_ptr<FrameMetaData> &other) override
    {
        // dynamic_cast cannot be used in this instance as it requires rtti. Chromium
        // disables rtti.
        auto otherptr = std::static_pointer_cast<FrameMetaDataWithRotation>(other);
        assert(otherptr);
        if(otherptr->get_timestamp() > m_timestamp) {
            m_rotation = otherptr->get_rotation();
            m_timestamp = otherptr->get_timestamp();
        }
    }

    int get_rotation() const {
        return m_rotation;
    }
protected:
    int m_rotation;
};

}
