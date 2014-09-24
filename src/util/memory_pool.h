/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "util/memory.h"

namespace lean {
/** \brief Auxiliary object for "recycling" allocated memory of fixed size */
class memory_pool {
    unsigned m_size;
    void *   m_free_list;
public:
    memory_pool(unsigned size):m_size(size), m_free_list(nullptr) {}
    ~memory_pool() {
        while (m_free_list != nullptr) {
            void * r = m_free_list;
            m_free_list = *(reinterpret_cast<void **>(r));
            free(r);
        }
    }

    void * allocate() {
        if (m_free_list != nullptr) {
            void * r = m_free_list;
            m_free_list = *(reinterpret_cast<void **>(r));
            return r;
        } else {
            return malloc(m_size);
        }
    }

    void recycle(void * ptr) {
        *(reinterpret_cast<void**>(ptr)) = m_free_list;
        m_free_list = ptr;
    }
};
}
