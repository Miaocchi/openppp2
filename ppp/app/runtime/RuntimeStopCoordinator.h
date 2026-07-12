#pragma once

#include <cstdint>
#include <mutex>

namespace ppp {
    namespace app {
        namespace runtime {

            class RuntimeStopCoordinator final {
            public:
                void BeginGeneration(std::uint64_t generation) noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    if (generation < generation_) {
                        return;
                    }
                    generation_ = generation;
                    stopping_ = false;
                    completed_ = false;
                    cleanup_success_ = true;
                }

                bool TryBeginStop(std::uint64_t generation) noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    if (generation != generation_ || stopping_ || completed_) {
                        return false;
                    }
                    stopping_ = true;
                    return true;
                }

                void CompleteStop(std::uint64_t generation, bool success) noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    if (generation != generation_ || !stopping_) {
                        return;
                    }
                    stopping_ = false;
                    completed_ = true;
                    cleanup_success_ = success;
                }

                bool IsStopping(std::uint64_t generation) const noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    return generation == generation_ && stopping_;
                }

                bool IsCompleted(std::uint64_t generation) const noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    return generation == generation_ && completed_;
                }

                bool WasCleanupSuccessful(std::uint64_t generation) const noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    return generation == generation_ && completed_ && cleanup_success_;
                }

            private:
                mutable std::mutex mutex_;
                std::uint64_t generation_ = 0;
                bool stopping_ = false;
                bool completed_ = false;
                bool cleanup_success_ = true;
            };

        }
    }
}
