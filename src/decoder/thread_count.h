#pragma once

#include <limits>
#include <thread>

namespace decoder_detail {
    inline int available_thread_count() noexcept {
        const unsigned int count = std::thread::hardware_concurrency();
        if (count < 2) {
            return 1;
        }
        const unsigned int maximum =
            static_cast<unsigned int>(std::numeric_limits<int>::max());
        return static_cast<int>(count > maximum ? maximum : count);
    }
}
