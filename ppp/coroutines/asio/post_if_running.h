#pragma once

#include <utility>

#include <boost/asio.hpp>

namespace ppp {
    namespace coroutines {
        namespace asio {
            /**
             * @brief Returns true when the io_context behind the executor has been stopped.
             * @details A stopped io_context never runs posted handlers, so a coroutine
             *          that suspends waiting for such a handler would leak with its stack.
             */
            inline bool                                                         ExecutorStopped(const boost::asio::io_context::executor_type& ex) noexcept {
                return ex.context().stopped();
            }

            template <class Executor>
            inline bool                                                         ExecutorStopped(const boost::asio::strand<Executor>& ex) noexcept {
                return ExecutorStopped(ex.get_inner_executor());
            }

            template <class Executor>
            inline bool                                                         ExecutorStopped(const Executor& ex) noexcept {
                // Polymorphic executors (any_io_executor): resolve the owning
                // execution context and treat a non-io_context owner as running.
                boost::asio::execution_context& owner = boost::asio::query(ex, boost::asio::execution::context_as<boost::asio::execution_context&>);
                boost::asio::io_context* context = dynamic_cast<boost::asio::io_context*>(&owner);
                return nullptr != context && context->stopped();
            }

            /**
             * @brief `boost::asio::post` that refuses to schedule on a stopped context.
             * @return false (handler not scheduled) when stopped; callers must then skip Suspend.
             */
            template <class Executor, class Handler>
            inline bool                                                         PostIfRunning(const Executor& ex, Handler&& handler) noexcept {
                if (ExecutorStopped(ex)) {
                    return false;
                }

                boost::asio::post(ex, std::forward<Handler>(handler));
                return true;
            }

            /** @brief `boost::asio::dispatch` counterpart of @ref PostIfRunning. */
            template <class Executor, class Handler>
            inline bool                                                         DispatchIfRunning(const Executor& ex, Handler&& handler) noexcept {
                if (ExecutorStopped(ex)) {
                    return false;
                }

                boost::asio::dispatch(ex, std::forward<Handler>(handler));
                return true;
            }
        }
    }
}
