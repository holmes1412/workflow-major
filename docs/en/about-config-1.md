# About global configuration

Global configuration is used to configure global default parameters to meet actual business requirements and improve program performance. Any modification of global configurations must be performed before any framework call is used, otherwise the modification may not valid. In addition, some global configuration options can be overridden by upstream configuration. For more information about this part of contents please refer to documents related to upstream.

# Change default configuration

WFGlobal.h contains the structure and default values of global configuration:

```cpp
struct WFGlobalSettings
{
    struct EndpointParams endpoint_params;
    unsigned int dns_ttl_default;   ///< in seconds, DNS TTL when network request success
    unsigned int dns_ttl_min;       ///< in seconds, DNS TTL when network request fail
    int dns_threads;
    int poller_threads;
    int handler_threads;
    int compute_threads;            ///< auto-set by system CPU number if value<=0
};


static constexpr struct WFGlobalSettings GLOBAL_SETTINGS_DEFAULT =
{
    .endpoint_params    =   ENDPOINT_PARAMS_DEFAULT,
    .dns_ttl_default    =   12 * 3600,
    .dns_ttl_min        =   180,
    .dns_threads        =   4,
    .poller_threads     =   4,
    .handler_threads    =   20,
    .compute_threads    =   -1,
};
```

Among all, the structure and default values of EndpointParams can be find at EndpointParams.h:

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

For example, change the default connection timeout to 5 seconds, default dns ttl to 1 hour, and increase the number of poller threads for message deserialization to 10:

```cpp
#include "workflow/WFGlobal.h"

int main()
{
    struct WFGlobalSettings settings = GLOBAL_SETTINGS_DEFAULT;

    settings.endpoint_params.connect_timeout = 5 * 1000;
    settings.dns_ttl_default = 3600;
    settings.poller_threads = 10;
    WORKFLOW_library_init(&settings);

    ...
}
```

The meaning of most parameters is clear. Pay attention to dnsttl related parameters, which are in seconds. While the endpoint related timeout parameters are in milliseconds, and they can use -1 to indicate unlimited.

dns_threads represents the number of threads accessing dns in parallel. Currently our dns is accessed through the system function getaddrinfo. If dns concurrency performance is needed, this value can be increased.

compute_threads represents the number of threads used for calculation, and the default -1 means it has the same number as current node CPU cores.

The two parameters related to network performance are poller_threads and handler_threads:

- The poller thread is mainly responsible for epoll (kqueue) and message deserialization.
- The handler thread is the thread where the network task callback and process are located.

All the resources required by the framework are applied for when they are used for the first time. For example, if the user does not use dns resolution, the dns thread will not be started.
