# About connection context

Connection context is an advanced topic of programming using this framework. A bit complicated in usage.

As noted in previous example, whether client or server task, we have no ways to specify the specific connection.

In some business scenarios, especially server, it may require to maintain the connection status. In other words, we need to bind a paragraph of context with connection.

Our framework provides users with a connection context mechanism.

# Application scenarios of connection context

Http protocol is like a completely connectionless protocol. Http session is implemented through cookie. This protocol is the friendliest type to our framework. So is kafka.

The connection between redis and mysql is obviously stateful. Redis specifies the database ID of current connection through SELECT command. Mysql is a thorough stateful connection.

When using the framework's redis or non-transactional mysql client task, as the URL already contains all the information related to connection selection, including:
  * Username & password
  * mysql character set
  * Database name or database number

The framework will automatically log in and select reusable connections based on this information, and the user does not need to care about the connection context.

This is why the SELECT command of redis and the USE command of mysql in the framework are disabled for users. To switch database, user need to create tasks using new URL.

Transactional mysql can have a fixed connection. For relevant details, please refer to the documents related to mysql.

However, if we implement a server of redis protocol, we need to know the status of the current connection.

# Method of using connection context

We need to emphasize that in general, only server task needs to use connection context, and only needs to be use it inside the process function, the is also the safest and simplest approach.

However, the task can also use or modify connection context in the callback, but you need to consider concurrency issue when using it. We will discuss the related issues in details.

Any network task can call the interface to obtain the connection object, and in turn obtain or modify the connection context. In [WFTask.h](../src/factory/WFTask.h), the call is as follows:

~~~cpp
template<class REQ, class, RESP>
class WFNetworkTask : public CommRequest
{
public:
    virtual WFConnection *get_connection() const;
    ...
};

Find more about the operation interfaces of the connection objects at [WFConneciton.h](../src/factory/WFConnection.h):
class WFConnection : public CommConnection
{
public:
    void *get_context() const;
    void set_context(void *context, std::function<void (void *)> deleter);
    void *test_set_context(void *test_context, void *new_context,
                           std::function<void (void *)> deleter);
};
~~~

get_connection() can only be called in process or callback. If it is called in callback, you need to check whether the return value is NULL.

If the WFConnection object is successfully obtained, you can then operate connection context. The connection context is a void * pointer, deleter is automatically called when the connection is closed.

# Timing and concurrency issues of accessing connection context

When client task is created, the connection object is not determined, so all client tasks use the connection context only in the callback.

Server task may use connection context in two places, i.e.: process and callback.

When using connection context in callback, you need to consider concurrency issues, because the same connection may be reused by multiple tasks and run to the callback at the same time.

Therefore, we recommend access or modify connection context in process function only. The connection will not be reused or released during the process. This is the simplest and safest method.

Note that the process we mentioned here only includes the inside of the process function. After the process function ends and before the callback, get_connection call always returns to NULL.

The test_set_context() in WFConnection is designed to solve concurrency issue of using the connection context in the callback, but we do not recommend using it.

In short, if you are not very familiar with the system implementation, please use connection context only in the process function of server task.

# Example: Reduce request header transmission of Http/1.1

Http protocol is like connection stateless protocol. Each request must send a complete header for the same connection.

Supposed that the cookie in the request is very large, this obviously increases the amount of data transmission. We can solve this problem through connection context on server-side.

We stipulated that the cookie in the http request is valid for all subsequent requests on this connection, and the cookie can no longer be sent in the subsequent request header.

The following are the server-side codes:

~~~cpp
void process(WFHttpTask *server_task)
{
    protocol::HttpRequest *req = server_task->get_req();
    protocol::HttpHeaderCursor cursor(req);
    WFConnection *conn = server_task->get_connection();
    void *context = conn->get_context();
    std::string cookie;

    if (cursor.find("Cookie", cookie))
    {
        if (context)
            delete (std::string *)context;
        context = new std::string(cookie);
        conn->set_context(context, [](void *p) { delete (std::string *)p; });
    }
    else if (context)
        cookie = *(std::string *)context;

    ...
}
~~~

In this way, we engage with the client that the cookie is transmitted only in the first request of the connection each time, which helps save traffic.

The implementation in client side needs to use a new callback function, the usages are as below:

~~~cpp
using namespace protocol;

void prepare_func(WFHttpTask *task)
{
    if (task->get_task_seq() == 0)
        task->get_req()->add_header_pair("Cookie", my_cookie);
}

int some_function()
{
    WFHttpTask *task = WFTaskFactory::create_http_task(...);
    static_cast<WFClientTask<HttpRequest, HttpResponse>>(task)->set_prepare(prepare_func);
    ...
}
~~~

In this example, when http task is the first request on the connection, we set the cookie. If it is not the first request, according to the agreement, no cookie will be set.

In addition, in prepare function, connection context can be used safely. On the same connection, prepare will not be concurrent.
