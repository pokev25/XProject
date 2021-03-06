#pragma once
#include "packet_handler_manager.h"

namespace XP
{

template <typename TSession>
class Session : public std::enable_shared_from_this<TSession>
{
public:
    typedef PacketHandlerManager<TSession> TPacketHandlerManager;
    static TPacketHandlerManager _s_packet_handler_manager;

public:
    explicit Session(boost::asio::io_service& ioservice);
    virtual ~Session();

protected:
    virtual void Shutdown(
        boost::asio::socket_base::shutdown_type shutdownType =
        boost::asio::socket_base::shutdown_type::shutdown_both) noexcept;

public:
    bool PostReceive();
    void PostWrite();

    template<typename T>
    bool SendPacket(const T& packet)
    {
        {
            LOCK_W(_lock);

            if (!_sendBuffer.SetPacket(packet))
            {
                LOG_ERROR(LOG_FILTER_PACKET_BUFFER, "Fail to SetPacket().");
                return false;
            }
        }

        PostWrite();
        return true;
    }

    boost::asio::ip::tcp::socket& GetSocket() { return _socket; }

protected:
    boost::asio::ip::tcp::socket _socket;

    PacketBuffer _recvBuffer;
    PacketBuffer _sendBuffer;

    SlimRWLock _lock;
};

template <typename TSession>
Session<TSession>::Session(boost::asio::io_service& ioservice)
    : _socket(ioservice)
{
}

template <typename TSession>
Session<TSession>::~Session()
{
    Shutdown();
}

namespace
{
std::string GetShutdownTypeString(boost::asio::socket_base::shutdown_type shutdownType)
{
    switch (shutdownType)
    {
    case boost::asio::socket_base::shutdown_type::shutdown_send:
        return std::string("shutdown_send");
    case boost::asio::socket_base::shutdown_type::shutdown_receive:
        return std::string("shutdown_receive");
    case boost::asio::socket_base::shutdown_type::shutdown_both:
        return std::string("shutdown_both");
    }

    return std::string("shutdown_unknown");
}
} // nonamed namespace

template <typename TSession>
void Session<TSession>::Shutdown(
    boost::asio::socket_base::shutdown_type shutdownType) noexcept
{
    if (!_socket.is_open())
        return;

    const auto& local_endpoint = _socket.local_endpoint();
    LOG_INFO(LOG_FILTER_SERVER, "Session is disconnected({}). ip: {}, port: {}",
        GetShutdownTypeString(shutdownType),
        local_endpoint.address().to_string(),
        local_endpoint.port());

    try
    {
        _socket.shutdown(shutdownType);
        _socket.close();
    }
    catch (const boost::system::error_code& errorCode)
    {
        LOG_ERROR(LOG_FILTER_SERVER, "Session fail to close socket."
            " error_code: {}, error_message: {}",
            errorCode.value(), errorCode.message());
    }
}

template <typename TSession>
bool Session<TSession>::PostReceive()
{
    if (!_socket.is_open())
    {
        LOG_ERROR(LOG_FILTER_CONNECTION, "Fail to PostReceive. Session is disconnected.");
        return false;
    }

    if (_recvBuffer.IsNotEnoughBuffer())
    {
        _recvBuffer.ReArrange();
    }

    auto self(shared_from_this());
    _socket.async_read_some(
        boost::asio::buffer(_recvBuffer.GetMutableBuffer(),
            static_cast<std::size_t>(_recvBuffer.GetRemainSize())),
        [this, selfMoved = std::move(self)]
    (const boost::system::error_code& errorCode, std::size_t bytes_transferred)
    {
        if (!_socket.is_open())
        {
            LOG_ERROR(LOG_FILTER_CONNECTION, "Fail to PostReceive."
                " Session is disconnected.");
            return;
        }

        const auto& remoteEndPoint = _socket.remote_endpoint();

        if (errorCode || (bytes_transferred == 0))
        {
            boost::asio::socket_base::shutdown_type shutdownType =
                boost::asio::socket_base::shutdown_type::shutdown_receive;

            if (errorCode == boost::asio::error::eof ||
                errorCode == boost::asio::error::connection_reset)
            {
                shutdownType =
                    boost::asio::socket_base::shutdown_type::shutdown_both;

                LOG_INFO(LOG_FILTER_CONNECTION, "Disconnected."
                    " address: {}, port: {}",
                    remoteEndPoint.address().to_string(), remoteEndPoint.port());
            }
            else if (errorCode == boost::asio::error::connection_aborted)
            {
                LOG_INFO(LOG_FILTER_CONNECTION, "Connection aborted."
                    " address: {}, port: {}",
                    remoteEndPoint.address().to_string(), remoteEndPoint.port());
            }
            else if (bytes_transferred == 0)
            {
                LOG_INFO(LOG_FILTER_CONNECTION, "Disconnected. 0 bytes transferred."
                    " address: {}, port: {}",
                    remoteEndPoint.address().to_string(), remoteEndPoint.port());
            }
            else
            {
                LOG_ERROR(LOG_FILTER_CONNECTION, "Connection receive error."
                    " error_code: {}, error_message: {}, address: {}, port: {}",
                    errorCode.value(), errorCode.message(),
                    remoteEndPoint.address().to_string(), remoteEndPoint.port());
            }

            Shutdown(shutdownType);
            return;
        }

        if (!_recvBuffer.AppendWriteSize(static_cast<uint16>(bytes_transferred)))
        {
            LOG_ERROR(LOG_FILTER_CONNECTION, "Receive buffer error."
                " recvBuffer remain size: {}, bytes_transferred: {}",
                _recvBuffer.GetRemainSize(), bytes_transferred);

            Shutdown(boost::asio::socket_base::shutdown_type::shutdown_receive);
            return;
        }

        while (_recvBuffer.IsAbleToGetPacket())
        {
            if (!_s_packet_handler_manager.Handle(
                static_cast<TSession&>(*this), _recvBuffer))
            {
                LOG_ERROR(LOG_FILTER_CONNECTION, "Receive Handler failed."
                    " packetNumber: {}",
                    _recvBuffer.GetPacketNo());

                Shutdown(boost::asio::socket_base::shutdown_type::shutdown_receive);
                return;
            }
        }

        PostReceive();
    });

    return true;
}

template <typename TSession>
void Session<TSession>::PostWrite()
{
    LOCK_W(_lock);

    if (!_socket.is_open())
    {
        LOG_ERROR(LOG_FILTER_CONNECTION, "Fail to PostWrite. socket is closed.");
        return;
    }

    if (_sendBuffer.IsEmptyData())
        return;

    auto self(shared_from_this());
    boost::asio::async_write(_socket,
        boost::asio::buffer(_sendBuffer.GetBuffer(), _sendBuffer.GetBufferSize()),
        [this, selfMoved = std::move(self)](
            const boost::system::error_code& errorCode, std::size_t bytes_transferred)
    {
        {
            LOCK_W(_lock);

            if (!_socket.is_open())
            {
                LOG_ERROR(LOG_FILTER_CONNECTION, "Fail to PostWrite."
                    " Session is disconnected.");
                return;
            }

            const auto& remoteEndPoint = _socket.remote_endpoint();

            if (errorCode || (bytes_transferred == 0))
            {
                boost::asio::socket_base::shutdown_type shutdownType =
                    boost::asio::socket_base::shutdown_type::shutdown_send;

                if (errorCode == boost::asio::error::eof ||
                    errorCode == boost::asio::error::connection_reset)
                {
                    shutdownType = boost::asio::socket_base::shutdown_type::shutdown_both;

                    LOG_INFO(LOG_FILTER_CONNECTION, "Disconnected."
                        " address: {}, port: {}",
                        remoteEndPoint.address().to_string(), remoteEndPoint.port());
                }
                else if (errorCode == boost::asio::error::connection_aborted)
                {
                    LOG_INFO(LOG_FILTER_CONNECTION, "Connection aborted."
                        " address: {}, port: {}",
                        remoteEndPoint.address().to_string(), remoteEndPoint.port());
                }
                else if (bytes_transferred == 0)
                {
                    LOG_INFO(LOG_FILTER_CONNECTION, "Disconnected. 0 bytes transferred."
                        " address: {}, port: {}",
                        remoteEndPoint.address().to_string(), remoteEndPoint.port());
                }
                else
                {
                    LOG_ERROR(LOG_FILTER_CONNECTION, "Connection send error."
                        " error_code: {}, error_message: {}, address: {}, port: {}",
                        errorCode.value(), errorCode.message(),
                        remoteEndPoint.address().to_string(), remoteEndPoint.port());
                }

                Shutdown(shutdownType);
                return;
            }

            _sendBuffer.TruncateBuffer(static_cast<uint16>(bytes_transferred));

            if (_sendBuffer.IsEmptyData())
            {
                _sendBuffer.ReArrange();
                return;
            }
        }

        PostWrite();
    });
}

} // namespace XP
