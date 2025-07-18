#pragma once

#include <Poco/Net/StreamSocket.h>

#include <Common/callOnce.h>
#include <Common/SSHWrapper.h>
#include <Common/SettingsChanges.h>
#include <Client/IServerConnection.h>
#include <Core/Defines.h>

#include <Formats/FormatSettings.h>

#include <IO/ReadBufferFromPocoSocketChunked.h>
#include <IO/WriteBufferFromPocoSocketChunked.h>

#include <Interpreters/TablesStatus.h>
#include <Interpreters/Context_fwd.h>

#include <Compression/ICompressionCodec.h>

#include <Storages/MergeTree/RequestResponse.h>

#include <optional>

#include "config.h"

namespace DB
{

struct Settings;
struct TimeoutSetter;

class Connection;
struct ConnectionParameters;
struct ClusterFunctionReadTaskResponse;

using ConnectionPtr = std::shared_ptr<Connection>;
using Connections = std::vector<ConnectionPtr>;

class NativeReader;
class NativeWriter;

/** Connection with database server, to use by client.
  * How to use - see Core/Protocol.h
  * (Implementation of server end - see Server/TCPHandler.h)
  *
  * As 'default_database' empty string could be passed
  *  - in that case, server will use it's own default database.
  */
class Connection : public IServerConnection
{
    friend class MultiplexedConnections;

public:
    Connection(const String & host_, UInt16 port_,
        const String & default_database_,
        const String & user_, const String & password_,
        const String & proto_send_chunked_, const String & proto_recv_chunked_,
        const SSHKey & ssh_private_key_,
        const String & jwt_,
        const String & quota_key_,
        const String & cluster_,
        const String & cluster_secret_,
        const String & client_name_,
        Protocol::Compression compression_,
        Protocol::Secure secure_,
        const String & bind_host_);

    ~Connection() override;

    IServerConnection::Type getConnectionType() const override { return IServerConnection::Type::SERVER; }

    static ServerConnectionPtr createConnection(const ConnectionParameters & parameters, ContextPtr context);

    /// Set throttler of network traffic. One throttler could be used for multiple connections to limit total traffic.
    void setThrottler(const ThrottlerPtr & throttler_) override
    {
        throttler = throttler_;
    }

    /// Change default database. Changes will take effect on next reconnect.
    void setDefaultDatabase(const String & database) override;

    void getServerVersion(const ConnectionTimeouts & timeouts,
                          String & name,
                          UInt64 & version_major,
                          UInt64 & version_minor,
                          UInt64 & version_patch,
                          UInt64 & revision) override;

    UInt64 getServerRevision(const ConnectionTimeouts & timeouts) override;

    const String & getServerTimezone(const ConnectionTimeouts & timeouts) override;
    const String & getServerDisplayName(const ConnectionTimeouts & timeouts) override;

    const SettingsChanges & settingsFromServer() const;

    /// For log and exception messages.
    const String & getDescription(bool with_extra = false) const override; /// NOLINT
    const String & getHost() const;
    UInt16 getPort() const;
    const String & getDefaultDatabase() const;

    Protocol::Compression getCompression() const { return compression; }

    std::vector<std::pair<String, String>> getPasswordComplexityRules() const override { return password_complexity_rules; }

    void sendQuery(
        const ConnectionTimeouts & timeouts,
        const String & query,
        const NameToNameMap& query_parameters,
        const String & query_id_/* = "" */,
        UInt64 stage/* = QueryProcessingStage::Complete */,
        const Settings * settings/* = nullptr */,
        const ClientInfo * client_info/* = nullptr */,
        bool with_pending_data/* = false */,
        const std::vector<String> & external_roles,
        std::function<void(const Progress &)> process_progress_callback) override;

    void sendQueryPlan(const QueryPlan & query_plan) override;

    void sendCancel() override;

    void sendData(const Block & block, const String & name/* = "" */, bool scalar/* = false */) override;

    void sendMergeTreeReadTaskResponse(const ParallelReadResponse & response) override;

    void sendExternalTablesData(ExternalTablesData & data) override;

    bool poll(size_t timeout_microseconds/* = 0 */) override;

    bool hasReadPendingData() const override;

    std::optional<UInt64> checkPacket(size_t timeout_microseconds/* = 0*/) override;

    Packet receivePacket() override;
    UInt64 receivePacketType() override;

    void forceConnected(const ConnectionTimeouts & timeouts) override;

    bool isConnected() const override { return connected && in && out && !in->isCanceled() && !out->isCanceled(); }

    bool checkConnected(const ConnectionTimeouts & timeouts) override { return isConnected() && ping(timeouts); }

    void disconnect() override;

    /// Send prepared block of data (serialized and, if need, compressed), that will be read from 'input'.
    /// You could pass size of serialized/compressed block.
    void sendPreparedData(ReadBuffer & input, size_t size, const String & name = "");

    void sendClusterFunctionReadTaskResponse(const ClusterFunctionReadTaskResponse & response);
    /// Send all scalars.
    void sendScalarsData(Scalars & data);
    /// Send parts' uuids to excluded them from query processing
    void sendIgnoredPartUUIDs(const std::vector<UUID> & uuids);

    TablesStatusResponse getTablesStatus(const ConnectionTimeouts & timeouts,
                                         const TablesStatusRequest & request);

    size_t outBytesCount() const { return out ? out->count() : 0; }
    size_t inBytesCount() const { return in ? in->count() : 0; }

    Poco::Net::Socket * getSocket() { return socket.get(); }

    /// Each time read from socket blocks and async_callback is set, it will be called. You can poll socket inside it.
    void setAsyncCallback(AsyncCallback async_callback_)
    {
        async_callback = std::move(async_callback_);
        if (in)
            in->setAsyncCallback(async_callback);
        if (out)
            out->setAsyncCallback(async_callback);
    }

    bool haveMoreAddressesToConnect() const { return have_more_addresses_to_connect; }

    void setFormatSettings(const FormatSettings & settings) override
    {
        format_settings = settings;
    }

private:
    String host;
    UInt16 port;
    String default_database;
    String user;
    String password;
    String proto_send_chunked;
    String proto_recv_chunked;
    String proto_send_chunked_srv;
    String proto_recv_chunked_srv;
#if USE_SSH
    SSHKey ssh_private_key;
#endif
    String quota_key;
#if USE_JWT_CPP && USE_SSL
    String jwt;
#endif

    /// For inter-server authorization
    String cluster;
    String cluster_secret;
    /// For DBMS_MIN_REVISION_WITH_INTERSERVER_SECRET
    String salt;
    /// For DBMS_MIN_REVISION_WITH_INTERSERVER_SECRET_V2
    std::optional<UInt64> nonce;

    /// Address is resolved during the first connection (or the following reconnects)
    /// Use it only for logging purposes
    std::optional<Poco::Net::SocketAddress> current_resolved_address;

    /// For messages in log and in exceptions.
    String description;
    String full_description;
    void setDescription();

    /// Returns resolved address if it was resolved.
    std::optional<Poco::Net::SocketAddress> getResolvedAddress() const;

    String client_name;

    bool connected = false;

    String server_name;
    UInt64 server_version_major = 0;
    UInt64 server_version_minor = 0;
    UInt64 server_version_patch = 0;
    UInt64 server_revision = 0;
    UInt64 server_parallel_replicas_protocol_version = 0;
    UInt64 server_cluster_function_protocol_version = 0;
    UInt64 server_query_plan_serialization_version = 0;
    String server_timezone;
    String server_display_name;
    SettingsChanges settings_from_server;

    std::unique_ptr<Poco::Net::StreamSocket> socket;
    std::shared_ptr<ReadBufferFromPocoSocketChunked> in;
    std::shared_ptr<WriteBufferFromPocoSocketChunked> out;
    std::optional<UInt64> last_input_packet_type;

    String query_id;
    Protocol::Compression compression;        /// Enable data compression for communication.
    Protocol::Secure secure;             /// Enable data encryption for communication.
    String bind_host;

    /// What compression settings to use while sending data for INSERT queries and external tables.
    CompressionCodecPtr compression_codec;

    /** If not nullptr, used to limit network traffic.
      * Only traffic for transferring blocks is accounted. Other packets don't.
      */
    ThrottlerPtr throttler;

    std::vector<std::pair<String, String>> password_complexity_rules;

    /// From where to read query execution result.
    std::shared_ptr<ReadBuffer> maybe_compressed_in;
    std::unique_ptr<NativeReader> block_in;
    std::unique_ptr<NativeReader> block_logs_in;
    std::unique_ptr<NativeReader> block_profile_events_in;

    /// Where to write data for INSERT.
    std::shared_ptr<WriteBuffer> maybe_compressed_out;
    std::unique_ptr<NativeWriter> block_out;

    bool have_more_addresses_to_connect = false;

    /// Logger is created lazily, for avoid to run DNS request in constructor.
    class LoggerWrapper
    {
    public:
        explicit LoggerWrapper(Connection & parent_)
            : log(nullptr), parent(parent_)
        {
        }

        LoggerPtr get()
        {
            callOnce(log_initialized, [&] {
                log = getLogger("Connection (" + parent.getDescription() + ")");
            });

            return log;
        }

    private:
        OnceFlag log_initialized;
        LoggerPtr log;
        Connection & parent;
    };

    LoggerWrapper log_wrapper;

    AsyncCallback async_callback = {};

    std::optional<FormatSettings> format_settings;

    void connect(const ConnectionTimeouts & timeouts);
    void sendHello(const Poco::Timespan & handshake_timeout);

    void cancel() noexcept;
    void reset() noexcept;

#if USE_SSH
    void performHandshakeForSSHAuth(const Poco::Timespan & handshake_timeout);
#endif

    void sendAddendum();
    void receiveHello(const Poco::Timespan & handshake_timeout);

#if USE_SSL
    void sendClusterNameAndSalt();
#endif
    bool ping(const ConnectionTimeouts & timeouts);

    Block receiveData();
    Block receiveLogData();
    Block receiveDataImpl(NativeReader & reader);
    Block receiveProfileEvents();

    String receiveTableColumns();
    std::unique_ptr<Exception> receiveException() const;
    Progress receiveProgress() const;
    ParallelReadRequest receiveParallelReadRequest() const;
    InitialAllRangesAnnouncement receiveInitialParallelReadAnnouncement() const;
    ProfileInfo receiveProfileInfo() const;

    void initInputBuffers();
    void initMaybeCompressedInput();
    void initBlockInput();
    void initBlockLogsInput();
    void initBlockProfileEventsInput();

    [[noreturn]] void throwUnexpectedPacket(TimeoutSetter & timeout_setter, UInt64 packet_type, const char * expected);
};

template <typename Conn>
class AsyncCallbackSetter
{
public:
    AsyncCallbackSetter(Conn * connection_, AsyncCallback async_callback) : connection(connection_)
    {
        connection->setAsyncCallback(std::move(async_callback));
    }

    ~AsyncCallbackSetter()
    {
        connection->setAsyncCallback({});
    }
private:
    Conn * connection;
};

}
