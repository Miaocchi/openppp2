/**
 * @file websocket_read_websocket.cpp
 * @brief Implements buffered read operations for WebSocket transport.
 */

#include <ppp/net/asio/websocket.h>
#include <ppp/net/asio/templates/SslSocket.h>
#include <ppp/net/asio/templates/WebSocket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/diagnostics/Error.h>
#include <boost/asio/bind_executor.hpp>

namespace ppp {
    namespace net {
        namespace asio {
            /**
             * @brief Reads exactly length bytes from WebSocket stream into caller buffer.
             * @param buffer Destination memory buffer.
             * @param offset Byte offset inside destination buffer.
             * @param length Number of bytes to read.
             * @param y Coroutine yield context for asynchronous read.
             * @return true if the read completes successfully; otherwise false.
             */
            bool websocket::Read(const void* buffer, int offset, int length, YieldContext& y) noexcept {
                if (NULLPTR == buffer || offset < 0 || length < 1) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::WebSocketReadInvalidArguments);
                    return false;
                }

                if (IsDisposed()) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionDisposed);
                    return false;
                }

                if (!websocket_.is_open()) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketDisconnected);
                    return false;
                }

                ppp::threading::Executors::ContextPtr context = context_;
                ppp::threading::Executors::StrandPtr strand = strand_;
                bool read_ok = false;
                boost::system::error_code read_ec;
                const YieldContext::Anchor anchor = y.Arm();
                if (!anchor) {
                    return false;
                }

                bool posted = ppp::threading::Executors::Post(context, strand,
                    [this, buffer, offset, length, anchor, &read_ok, &read_ec, strand]() noexcept {
                        auto complete = [anchor, length, &read_ok, &read_ec](const boost::system::error_code& ec, std::size_t sz) noexcept {
                            read_ec = ec;
                            read_ok = boost::system::errc::success == ec && static_cast<std::size_t>(length) == sz;
                            anchor.R();
                        };
                        if (strand) {
                            boost::asio::async_read(websocket_, boost::asio::buffer((Byte*)buffer + offset, length),
                                boost::asio::bind_executor(*strand, std::move(complete)));
                        }
                        else {
                            boost::asio::async_read(websocket_, boost::asio::buffer((Byte*)buffer + offset, length), std::move(complete));
                        }
                    });

                if (!posted) {
                    if (ppp::diagnostics::ErrorCode::Success == ppp::diagnostics::GetLastErrorCode()) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RuntimeTaskPostFailed);
                    }
                    return false;
                }

                y.Suspend(anchor);
                if (read_ok) {
                    return true;
                }

                if (boost::asio::error::operation_aborted == read_ec ||
                    boost::asio::error::eof == read_ec ||
                    boost::beast::websocket::error::closed == read_ec)
                {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::WebSocketClosed);
                    return false;
                }

                if (boost::asio::error::timed_out == read_ec) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketTimeout);
                }
                elif (boost::asio::error::connection_reset == read_ec ||
                      boost::asio::error::connection_aborted == read_ec ||
                      boost::asio::error::not_connected == read_ec ||
                      boost::asio::error::broken_pipe == read_ec ||
                      boost::asio::error::shut_down == read_ec)
                {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketDisconnected);
                }
                elif (boost::asio::error::invalid_argument == read_ec ||
                      boost::asio::error::message_size == read_ec)
                {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolFrameInvalid);
                }
                elif (boost::beast::websocket::make_error_code(boost::beast::websocket::error::closed).category() == read_ec.category()) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::WebSocketReadFailed);
                }
                else {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketReadFailed);
                }

                return false;
            }
        }
    }
}
