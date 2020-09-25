# About DNS

DNS (Domain Name Service Protocol) is a distributed network directory service, mainly used for the mutual conversion between domain names and IP addresses.

During communication access, DNS resolution is required for non-IP domain names. This process is the conversion process from domain names to IP addresses.

DNS resolution is a relatively large consumption. Both server and local operating system usually have their own DNS Cache to reduce unnecessary requests.

Some programs may be designed with their own DNS Cache in their own process, including popular browsers, communication frameworks, etc. Workflow is also designed with its own DNS Cache. This part of DNS is completely taken over and "hidden" by the framework for user’s convenience.

### TTL

Time To Live, also known as TTL, means the time that DNS records are cached on the DNS Cache.

### DNS approach of the framework

At present, the system function getaddrinfo is directly called to obtain the result, below are some detailed information:

1. When the frame's own DNS Cache is hit and the TTL is valid, DNS resolution will not occur.
2. When the domain name is ipv4, ipv6 and unix-domain-socket, DNS resolution will not occur.
3. DNS resolution is a special computing task, which is encapsulated into WFThreadTask.
4. DNS resolution uses a completely independent and isolated thread pool, that is to say, it does not occupy computing thread pool nor communication thread pool.

We are considering adding UDP requests to DNS Server to obtain results in the near future.

### Global DNS configuration

In the file [WFGlobal.h](../src/manager/WFGlobal.h), you can see one of our global configuration information: 

~~~cpp
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

static constexpr struct WFGlobalSettings GLOBAL_SETTING_DEFAULT =
{
    .endpoint_params    =    ENDPOINT_PARAMS_DEFAULT,
    .dns_ttl_default    =    12 * 3600,  /* in seconds */
    .dns_ttl_min        =    180,        /* reacquire when communication error */
    .dns_threads        =    4,
    .poller_threads     =    4,
    .handler_threads    =    20,
    .compute_threads    =    -1
};
//compute_threads<=0 means auto-set by system cpu number
~~~

Among all, DNS related configurations include:

  * dns_threads: the number of threads in DNS thread pool, the default value is 4.
  * dns_ttl_default: The default TTL in DNS Cache, which is in second unit and 12 hours by default. dns cache is of the current process, namely the process will disappear when the process exits, and the configuration is only valid for the current process.
  * dns_ttl_min: The minimum TTL of dns, which is in seconds, the default value is 3 minutes, and it is used for the decision on whether to retry dns after communication failure.
  
Simply speaking, TTL is checked in each communication to decide whether to retry DNS resolution.

Dns_ttl_default is checked by default, and dns_ttl_min will be checked only when retry after communication fails.

Global DNS configuration can be overridden by a separate address configuration through upstream function.

Each AddressParams of Upstream also has dns_ttl_default and dns_ttl_min configuration items, which are used in a way which is similar to that of Global. For details of the structure at [upstream document](./about-upstream.md#Address属性).

### Processing of TTL expiration moments in high concurrency scenarios

If this domain name receives a large number of concurrent requests at the moment when TTL expiries, and one domain name may face a large number of DNS resolutions at the same time.

The framework uses self-consistent logic to reasonably avoid/reduce this possibility:

  * When getting the result from DNS Cache, if TTL expires, the TTL expiration time will be directly increased by 10 seconds, and then the expiry result will be returned. This series of process takes place under the protection of a mutex.
  * If a large number of requests flood in at the moment when TTL expiries, under the protection of this mutually exclusion logic, [the first one] that is found expired will get expiry result and initiates DNS resolution, while all other requests will continue to use the old result within 10 seconds.
  * As long as [the first one] successfully completes new DNS resolution within 10 seconds, the DNS Cache can be updated to ensure the correctness of the logic; in the next 10 seconds, one more (only one more) DNS resolution will be carried out.
  * In every 10 seconds, the "just" expired domain name will receive one more (only one more) DNS resolution.
  * In order to prevent this mutual exclusion logic from affecting performance, the framework uses a double check lock mode to accelerate processing and effectively avoid mutex competition.
  * Again, please note that it is only valid for the "just" expired domain name, and doesn’t work on those having expired long ago.
  * To know more, please refer to the source code at DNSCache.
  
The framework has the following two scenarios that will face a large number of DNS resolutions towards the same domain name at the same time:

1. The program has just started, and a large number of requests are initiated towards the same domain name at a moment.
2. A large number of requests are initiated towards a domain name not accessed for a long time (far longer than TTL) suddenly at a moment.

The framework believes that the two scenarios are acceptable. Rather, the large number of DNS requests in such scenarios is completely reasonable and logically rigorous.
