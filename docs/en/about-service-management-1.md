# About service governance

We have a full set of mechanism to manage the services. This mechanism includes the following functions:

  * User-level DNS.
  * Selection of service address
    * Including multiple selection mechanisms, such as weight random, consistent hash, user-specified selection methods, etc.
  * Fusing and restoration of services.
  * Load balancing.
  * Independent parameter configuration of a single service.
  * Master-slave relationship of service, etc.
  
All these functions depend on our upstream subsystem. By making good use of this system, we can easily implement more complex service grid functions.

# Upstream name

The upstream name is equivalent to the domain name inside the program, but compared to general domain names, upstream has more functions, including:

  * Domain name generally can only point to a group of ip addresses, while upstream name can point to a group of ip addresses or domain names.
  * The object (domain name or ip) that upstream points to can contain port information.
  * Upstream has powerful functions of managing and selecting targets, and each target can contain a large number of attributes.
  * Upstream updates are real-time and completely thread-safe, while the DNS information of domain names cannot be updated in real time.
  
In terms of implementation, if there is no need to access external network, upstream can completely replace the domain name and DNS.

# Create and delete upstream

For details of creation interfaces in upstream, please refer to [UpstreamMananer.h](/src/manager/UpstreamMananer.h):

~~~cpp
using upstream_route_t = std::function<unsigned int (const char *, const char *, const char *)>;

class UpstreamManager
{
public:
    static int upstream_create_consistent_hash(const std::string& name,
                                               upstream_route_t consitent_hash);

    static int upstream_create_weighted_random(const std::string& name,
                                               bool try_another);

    static int upstream_create_manual(const std::string& name,
                                      upstream_route_t select,
                                      bool try_another,
                                      upstream_route_t consitent_hash);

    static int upstream_delete(const std::string& name);
    ...
};
~~~

These three functions are three types of upstream respectively: consistent hash, weight random and user manual selection.

The parameter name is the upstream name. After creation, the name can be used as a domain name.

The consistent_hash and select parameters are in one type, namely std::function of upstream_route_t, which is used to specify the routing method.

Try_another means whether to continue trying to find an available target if the selected target is not usable (fuse). The consistent_hash mode does not have this attribute.

The 3 parameters received by upstream_route_t parameter are the path, query and fragment parts of URL. For example, if the URL is: http://abc.com/home/index.html?a=1#bottom 

Then the three parameters are "/home/index.html", "a=1" and "bottom" respectively. Users can select target server based on these three parts, or perform consistent hash.

Please note that in the above interface, all consistent_hash parameters can pass nullptr, and we will use the default consistent hash algorithm.

# Example 1: Weight distribution

We want to make 50% of the request for visiting www.sogou.com to two addresses, hit 127.0.0.1:8000 and 127.0.0.1:8080, and make their load to be 1:4.

We don't have to care about how many IP addresses there are under the domain name www.sogou.com. In short, the actual domain name will receive 50% of the requests.

~~~cpp
#include "workflow/UpstreamManager.h"
#include "workflow/WFTaskFactory.h"

int main()
{
    UpstreamManager::upstream_create_weighted_random("www.sogou.com", false);
    struct AddressParams params = ADDRESS_PARAMS_DEFAULT;

    params.weight = 5;
    UpstreamManager::upstream_add_server("www.sogou.com", "www.sogou.com", &params);
    params.weight = 1;
    UpstreamManager::upstream_add_server("www.sogou.com", "127.0.0.1:8000", &params);
    params.weight = 4;
    UpstreamManager::upstream_add_server("www.sogou.com", "127.0.0.1:8080", &params);

    WFHttpTask *task = WFTaskFactory::create_http_task("http://www.sogou.com/index.html", ...);
    ...
}
~~~

Please note that the above functions can be called in any scenario, and they are completely thread-safe, and take effect in real time.

Once the task is created, it means the upstream target selection has been completed. In http task, if the selected target is 127.0.0.1:8000,

Then, the Host header content in the request is 127.0.0.1:8000 instead of www.sogou.com. So, if necessary, you can modify as:

~~~cpp
    WFHttpTask *task = WFTaskFactory::create_http_task("http://www.sogou.com/index.html", ...);
    task->get_req()->set_header_pair("Host", "www.sogou.com");  
~~~

In addition, since all our protocols, including user-defined protocols, have URLs, the upstream function can be applied to all protocols.

# Example 2: Manual selection

In the above example, we want to make the request whose query is "123" in the url and hit 127.0.0.1:8000, or hit 8080 port if it is "abc", or hit normal domain names for others.

~~~cpp
#include "workflow/UpstreamManager.h"
#include "workflow/WFTaskFactory.h"

int my_select(const char *path, const char *query, const char *fragment)
{
    if (strcmp(query, "123") == 0)
        return 1;
    else if (strcmp(query, "abc") == 0)
        return 2;
    else
        return 0;
}

int main()
{
    UpstreamManager::upstream_create_manual("www.sogou.com", my_select, false, nullptr);

    UpstreamManager::upstream_add_server("www.sogou.com", "www.sogou.com");
    UpstreamManager::upstream_add_server("www.sogou.com", "127.0.0.1:8000");
    UpstreamManager::upstream_add_server("www.sogou.com", "127.0.0.1:8080");

    /* This URL will route to 127.0.0.1:8080 */
    WFHttpTask *task = WFTaskFactory::create_http_task("http://www.sogou.com/index.html?abc", ...);
    ...
}
~~~

Since we provide built-in redis and mysql protocols, this method can be used to implement the database read-write separation function very conveniently (note: non-transactional operation).

In the above two examples, the upstream name used is www.sogou.com, which is indeed also a domain name. Of course, users can use the string sogou more simply, if so, when the task is created as:

~~~cpp
    WFHttpTask *task = WFTaskFactory::create_http_task("http://sogou/home/1.html?abc", ...);
~~~

In short, if the host part of the url is an upstream already created, it will be used as an upstream.

~~~cpp
    WFHttpTask *task = WFTaskFactory::create_http_task("http://sogou/home/1.html?abc", ...);
~~~

# Example 3: Consistent hash

In this scenario, we need to randomly select a machine for communication from 10 redis instances. But we guarantee that the same URL must visit a definite target. The method is simple:

~~~cpp
int main()
{
    UpstreamManager::upstream_create_consistent_hash("redis.name", nullptr);

    UpstreamManager::upstream_add_server("redis.name", "10.135.35.53");
    UpstreamManager::upstream_add_server("redis.name", "10.135.35.54");
    UpstreamManager::upstream_add_server("redis.name", "10.135.35.55");
    ...
    UpstreamManager::upstream_add_server("redis.name", "10.135.35.62");

    auto *task = WFTaskFactory::create_redis_task("redis://:mypassword@redis.name/2?a=hello#111", ...);
    ...
}
~~~

Our redis task does not identify the query part, and users can fill in it at will. The 2 in the path part represents the redis library number.

At this time, the consistent_hash function will get three parameters, i.e.: "/2", "a=hello" and "111", but because we use nullptr, the default consistent hash will be called.

The server in upstream does not specify a port number, so the port in the url will be used. Redis is 6379 by default.

Consitent_hash does not have a try_another option, if the target is blown, another one will be selected automatically. The same url will also get the same choice (cache friendly).

# Parameters of upstream server

In example 1, we set the weight of the server through params parameter setting. Of course, the server has many parameters instead of weight only. This structure is defined as follows:

~~~cpp
// In EndpointParams.h
struct EndpointParams
{
    size_t max_connections;
    int connect_timeout;
    int response_timeout;
    int ssl_connect_timeout;
};

// In UpstreamMananger.h
struct AddressParams
{
    struct EndpointParams endpoint_params; ///< Connection config
    unsigned int dns_ttl_default;          ///< in seconds, DNS TTL when network request success
    unsigned int dns_ttl_min;              ///< in seconds, DNS TTL when network request fail
/**
 * - The max_fails directive sets the number of consecutive unsuccessful attempts to communicate with the server.
 * - After 30s following the server failure, upstream probe the server with some live client’s requests.
 * - If the probes have been successful, the server is marked as a live one.
 * - If max_fails is set to 1, it means server would out of upstream selection in 30 seconds when failed only once
 */
    unsigned int max_fails;                ///< [1, INT32_MAX] max_fails = 0 means max_fails = 1
    unsigned short weight;                 ///< [1, 65535] weight = 0 means weight = 1. only for master
#define SERVER_TYPE_MASTER    0
#define SERVER_TYPE_SLAVE     1
    int server_type;                       ///< default is SERVER_TYPE_MASTER
    int group_id;                          ///< -1 means no group. Slave without group will backup for any master
};
~~~

The role of most parameters is clear at a glance. Among all, the parameters related to endpoint_params and dns can cover the global configuration.

For example, for each target ip, the global maximum number of connections is 200, but I want to set the maximum number of connections to 1000 for 10.135.35.53. If so, you can do this way:

~~~cpp
    UpstreamManager::upstream_create_weigthed_random("10.135.35.53", false);
    struct AddressParams params = ADDRESS_PARAMS_DEFAULT;
    params.endpoint_params.max_connections = 1000;
    UpstreamManager::upstream_add_server("10.135.35.53", "10.135.35.53", &params);
~~~

Max_fails parameter represents the maximum number of errors. If the selected target continuously fails to reach max_fails, it will be blown. If the upstream’s try_another attribute is false, the task will fail.

In the task callback, get_state()=WFT_STATE_TASK_ERROR, get_error()=WFT_ERR_UPSTREAM_UNAVAILABLE.

If try_another is true and all servers are blown, you will get the same error. The fusing time is 30 seconds.

Server_type and group_id are used for master-slave functions. All upstream must have a server classified as MASTER, otherwise upstream will not be available.

The server classified as SLAVE will be used when the MASTER with the same group_id is blown.

For more information about upstream functions, please refer to: [about-upstream.md](/docs/about-upstream.md).
