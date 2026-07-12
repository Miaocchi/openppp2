#pragma once

#include <ppp/stdafx.h>

#include <mutex>

namespace ppp {
    namespace app {
        namespace runtime {

            class RuntimeStopCoordinator final {
            public:
                void BeginGeneration(uint64_t generation) noexcept;
                bool TryBeginStop(uint64_t generation) noexcept;
                void CompleteStop(uint64_t generation, bool success) noexcept;
                bool IsStopping(uint64_t generation) const noexcept;
                bool IsCompleted(uint64_t generation) const noexcept;

            private:
                mutable std::mutex mutex_;
                uint64_t generation_ = 0;
                bool stopping_ = false;
                bool completed_ = false;
                bool cleanup_success_ = true;
            };

        }
    }
}
