# About timeout

In order to allow all communication tasks to run accurately as expected, we provide a large number of timeout configuration functions to ensure the accuracy of timeouts.

Some of these timeout configurations are global, such as connection timeout, while you can also configure your own connection timeout for a domain name through the upstream function.

Some timeouts are task-level, such as the timeout for sending a complete message. Because the user needs to dynamically configure this value according to the size of the message.

Of course, for the server, it has its own overall timeout configuration. In short, timeout is a complicated issue so we shall process as accurate as possible.

All timeouts leverage poll style, namely int type in milliseconds, -1 represents unlimited.

In addition, as we noted in the project introduction, you can ignore all the configurations, and don’t have to adjust unless encounters actual needs.

### Basic communication timeout configuration

In the file EndpointParams.h, we can see:
```cpp
struct EndpointParams
{
    size_t max_connections;
    int connect_timeout;
    int response_timeout;
    int ssl_connect_timeout;
};

static constexpr struct EndpointParams ENDPOINT_PARAMS_DEFAULT =
{
    .max_connections        = 200,
    .connect_timeout        = 10 * 1000,
    .response_timeout       = 10 * 1000,
    .ssl_connect_timeout    = 10 * 1000,
};
```

Among all, the configurations related to timeout include the following 3 items.   

- connect_timeout: The timeout for creating a connection with the target. The default value is 10 seconds.
- response_timeout: The timeout to wait for the target response is 10 seconds by default. It represents the timeout for successfully sending to the target or reading a piece of data from the target.
- ssl_connect_timeout: The timeout for completing SSL handshake with the target. The default value is 10 seconds.

This structure is the most basic configuration of communication connection, and almost all subsequent communication configurations will contain this structure.

# Global timeout configuration

In the file WFGlobal.h, we can see one of our global configuration information:

```cpp
struct WFGlobalSettings
{
    EndpointParams endpoint_params;
    unsigned int dns_ttl_default;
    unsigned int dns_ttl_min;
    int dns_threads;
    int poller_threads;
    int handler_threads;
    int compute_threads;
};

static constexpr struct WFGlobalSettings GLOBAL_SETTINGS_DEFAULT =
{
    .endpoint_params    =    ENDPOINT_PARAMS_DEFAULT,
    .dns_ttl_default    =    12 * 3600,    /* in seconds */
    .dns_ttl_min        =    180,          /* reacquire when communication error */
    .dns_threads        =    8,
    .poller_threads     =    2,
    .handler_threads    =    20,
    .compute_threads    =    -1
};
//compute_threads<=0 means auto-set by system cpu number
```

Among all, the configuration related to timeout is EndpointParamsendpoint_params

The way to modify the global configuration is to perform operations similar to the following before calling any of the factory functions:

```cpp
int main()
{
    struct WFGlobalSettings settings = GLOBAL_SETTINGS_DEFAULT;
    settings.endpoint_params.connect_timeout = 2 * 1000;
    settings.endpoint_params.response_timeout = -1;
    WORKFLOW_library_init(&settings);
}
```

In the above example, the connection timeout is changed to 2 seconds, and the server response timeout is unlimited. In this configuration, the timeout for receiving the complete message must be configured for each task, otherwise the waiting will be infinite.

The global timeout configuration can be overwritten by a separate address configuration through upstream function. For example, you can specify the connection timeout for a certain domain name.

Each AddressParams of Upstream also has an EndpointParamsendpoint_params, which is used in the same way as Global. For details of the structure at upstream document.

# Server timeout configuration

In the http_proxy example, we introduced server start configuration. The configuration related to timeout includes:

- peer_response_timeout: The definition is the same as that of global peer_response_timeout, which means the response timeout of remote client. The default value is 10 seconds.
- receive_timeout: The timeout for receiving a complete request is -1 by default.
- keep_alive_timeout: Connection duration is 1 minute by default. The timeout for Redis server is 5 minutes.
- ssl_accept_timeout: The timeout for completing ssl handshake is 10 seconds by default.

In this default configuration, the client can send a byte every 9 seconds, so that the server could keep receiving without causing a timeout. Therefore, if the service is used on the public network, receive_timeout needs to be configured.

# Task-level timeout configuration

The task-level timeout configuration is accomplished through several interface calls of network task:

```cpp
template <class REQ, class RESP>
class WFNetworkTask : public CommRequest
{
...
public:
    /* All in milliseconds. timeout == -1 for unlimited. */
    void set_send_timeout(int timeout) { this->send_timeo = timeout; }
    void set_receive_timeout(int timeout) { this->receive_timeo = timeout; }
    void set_keep_alive(int timeout) { this->keep_alive_timeo = timeout; }
...
}
```

Among all, set_send_timeout() sets the timeout for sending a complete message, and the default value is -1.
set_receive_timeout() is only valid for client tasks, and it means the timeout for receiving a complete server reply. The default value is -1.

- receive_timeout of server task is in server start configuration. All server tasks processed by users have successfully received complete requests.

set_keep_alive() interface sets the connection keep timeout. In general, the framework can process the connection retention problem well, and the user does not need to call it.

In case of http protocol, client or server wants to use short connection, it can be completed by adding HTTP header, try to avoid using this interface to make modifications.

If a redis client wants to close the connection after the request, you need to use this interface. Obviously, set_keep_alive() is invalid in the callback (the connection has been reused).

# Task synchronization waiting timeout

There is a very special timeout configuration, which is the only synchronization waiting timeout in the Global. We do not encourage using it, but it can achieve good results in certain application scenarios.

In the current framework, the target server has upper connection limit (configurable for both global and upstream). If the connection has reached the upper limit, by default, the client task will fail and return.

In callback, task->get_state() gets WFT_STATE_SYS_ERROR, task->get_error() gets EAGAIN. If the task is configured with retry, it will automatically initiate a retry.

It’s allowed to configure a synchronization waiting timeout through the task->set_wait_timeout() interface. If a connection is released during this period, the task can occupy the connection.

If the user configures wait_timeout and does not get the connection before the timeout, the callback will get the WFT_STATE_SYS_ERROR status and ETIMEDOUT error.

```cpp
class CommRequest : public SubTask, public CommSession
{
public:
    ...
    void set_wait_timeout(int wait_timeout) { this->wait_timeout = wait_timeout; }
}
```

# Check timeout reason
The communication task includes a get_timeout_reason() interface, which is used to return the timeout reason, but it doesn’t provide too much details. It includes the following return values:
  * TOR_NOT_TIMEOUT: Not a timeout.
  * TOR_WAIT_TIMEOUT: Synchronization waiting timeout.
  * TOR_CONNECT_TIMEOUT: Connection timeout. It is the same timeout value for TCP, SCTP and other protocol connections as well as SSL connection.
  * TOR_TRANSMIT_TIMEOUT: All transmissions timeout. It cannot further distinguish whether it is in sending phase or receiving phase. It may be refined in the future.
    * server task, the timeout reason must be TRANSMIT_TIMEOUT, and it must be in the stage of sending a reply.
    
# Implement timeout function

Inside the framework, there are more types of timeouts that need to be processed than we list here. Except for wait_timeout, all depend on Linux timer_fd, one for each epoll thread.

In the default configuration, the number of epoll threads is 2, which can meet the needs of most applications.

The current timeout algorithm uses linked list + Red-Black Tree data structure, and the time complexity ranges between O(1) and O(logn), where n represents the fd number of epoll thread.

Timeout handline is currently not the bottleneck, because epoll-related calls of Linux kernel are also O(logn) time complexity. It makes no difference if we made all the timeouts to be O(1).
