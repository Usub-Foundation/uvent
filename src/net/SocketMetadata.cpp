//
// Created by root on 9/13/25.
//

#include "uvent/net/SocketMetadata.h"
#include "uvent/pool/SocketHeaderPool.h"

namespace usub::uvent::net
{
    void delete_header(void* ptr)
    {
        // delete static_cast<SocketHeader*>(ptr);
        pool::g_header_pool.release(static_cast<SocketHeader*>(ptr));
    }
} // namespace usub::uvent::net
