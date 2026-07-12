#pragma once

#include <ppp/app/runtime/RuntimeSnapshot.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ppp {
    namespace app {
        namespace runtime {

            /**
             * @brief Lightweight in-process runtime snapshot notifier.
             *
             * Callbacks run outside the publisher mutex so listeners may
             * subscribe/unsubscribe/re-publish without deadlock.
             */
            class RuntimeSnapshotPublisher final {
            public:
                using Listener = std::function<void(const RuntimeSnapshot&)>;

                RuntimeSnapshotPublisher() = default;

                void Publish(RuntimeSnapshot snapshot) noexcept {
                    RuntimeSnapshot snapshot_copy;
                    std::vector<Listener> callbacks;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        latest_ = std::move(snapshot);
                        snapshot_copy = latest_;
                        callbacks.reserve(listeners_.size());
                        for (const auto& item : listeners_) {
                            callbacks.emplace_back(item.second);
                        }
                    }

                    for (const Listener& callback : callbacks) {
                        if (!callback) {
                            continue;
                        }
                        try {
                            callback(snapshot_copy);
                        }
                        catch (...) {
                        }
                    }
                }

                std::uint64_t Subscribe(Listener listener) noexcept {
                    if (!listener) {
                        return 0;
                    }

                    std::lock_guard<std::mutex> scope(mutex_);
                    const std::uint64_t token = next_listener_token_++;
                    listeners_.emplace(token, std::move(listener));
                    return token;
                }

                void Unsubscribe(std::uint64_t token) noexcept {
                    if (0 == token) {
                        return;
                    }

                    std::lock_guard<std::mutex> scope(mutex_);
                    listeners_.erase(token);
                }

                RuntimeSnapshot GetLatest() const noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    return latest_;
                }

            private:
                mutable std::mutex                          mutex_;
                RuntimeSnapshot                             latest_;
                std::unordered_map<std::uint64_t, Listener> listeners_;
                std::uint64_t                               next_listener_token_ = 1;
            };

        }
    }
}
