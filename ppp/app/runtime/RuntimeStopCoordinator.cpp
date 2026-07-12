#include <ppp/app/runtime/RuntimeStopCoordinator.h>

namespace ppp {
    namespace app {
        namespace runtime {

            void RuntimeStopCoordinator::BeginGeneration(uint64_t generation) noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                if (generation < generation_) {
                    return;
                }
                generation_ = generation;
                stopping_ = false;
                completed_ = false;
                cleanup_success_ = true;
            }

            bool RuntimeStopCoordinator::TryBeginStop(uint64_t generation) noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                if (generation != generation_ || stopping_ || completed_) {
                    return false;
                }
                stopping_ = true;
                return true;
            }

            void RuntimeStopCoordinator::CompleteStop(uint64_t generation, bool success) noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                if (generation != generation_ || !stopping_) {
                    return;
                }
                stopping_ = false;
                completed_ = true;
                cleanup_success_ = success;
            }

            bool RuntimeStopCoordinator::IsStopping(uint64_t generation) const noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                return generation == generation_ && stopping_;
            }

            bool RuntimeStopCoordinator::IsCompleted(uint64_t generation) const noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                return generation == generation_ && completed_;
            }

        }
    }
}
