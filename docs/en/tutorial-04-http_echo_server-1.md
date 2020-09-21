# The first server: http_echo_server

# Sample code
[tutorial-04-http\_echo\_server.cc](../tutorial/tutorial-04-http_echo_server.cc)

# About http_echo_server

This is an http server. Return to an html page and display the header information of the http request sent by the browser.   

The program log will print the requested client address and requested sequence number (the number of the requests on current connection). When 10 requests are completed on the same connection, the server will actively close the connection.   

The program finishes normally with Ctrl-C, and all resources are completely recovered.   

# Create and start http server

In this example, we use the default parameters of http server. The creation and start process is very simple.

```cpp
WFHttpServer server(process);
port = atoi(argv[1]);
if (server.start(port) == 0)
{
    pause();
    server.stop();
}
...
```

This process is very simple. Please note that start is non-blocking, so you need to pause the program. Obviously, you can also start multiple server objects and then pause.   

Once the server starts, you can stop the server at any time through the stop() interface. Stop will wait till the execution of the request being served is finished.   

Accordingly, stop is a blocking operation. If you need a non-blocking shutdown, you can use the shutdown+wait_finish interface.   

The start() interface has several overload functions. In [WFServer.h](../src/server/WFServer.h), you can see the following interfaces:   

```cpp
class WFServerBase
{
public:
    /* To start TCP server. */
    int start(unsigned short port);
    int start(int family, unsigned short port);
    int start(const char *host, unsigned short port);
    int start(int family, const char *host, unsigned short port);
    int start(const struct sockaddr *bind_addr, socklen_t addrlen);

    /* To start an SSL server */
    int start(unsigned short port, const char *cert_file, const char *key_file);
    int start(int family, unsigned short port,
              const char *cert_file, const char *key_file);
    int start(const char *host, unsigned short port,
              const char *cert_file, const char *key_file);
    int start(int family, const char *host, unsigned short port,
              const char *cert_file, const char *key_file);
    int start(const struct sockaddr *bind_addr, socklen_t addrlen,
              const char *cert_file, const char *key_file);

    /* For graceful restart. */
    int serve(int listen_fd);
    int serve(int listen_fd, const char *cert_file, const char *key_file);
};
```

These interfaces are relatively easy to understand. Among all, when starting SSL server, cert_file and key_file are in PEM format.   

The last two serve() interfaces with listen_fd are mainly used for graceful restart. Or simply create a server with a non-TCP protocol (such as SCTP).   

One thing deserves special attention is that, one server object corresponds to one listen_fd. If you run the server on both IPv4 and IPv6 protocols, you need:   
```
{
    WFHttpServer server_v4(process);
    WFHttpServer server_v6(process);
    server_v4.start(AF_INET, port);
    server_v6.start(AF_INET6, port);
    ...
    // now stop...
    server_v4.shutdown();   /* shutdown() is nonblocking */
    server_v6.shutdown();
    server_v4.wait_finish();
    server_v6.wait_finish();
}
```

We cannot share the connection count between two servers. Therefore, we recommend you to start the IPv6 server only, because it can accept the connections of IPv4.

# Business logic of http echo server

A process parameter is transferred in when constructing http server, which is also a std::function. Defined as:

```cpp
using http_process_t = std::function<void (WFHttpTask *)>;
using WFHttpServer = WFServer<protocol::HttpRequest, protocol::HttpResponse>;

template<>
WFHttpServer::WFServer(http_process_t proc) :
    WFServerBase(&HTTP_SERVER_PARAMS_DEFAULT),
    process(std::move(proc))
{
}
```

The http_proccess_t and http_callback_t are the same in type. Both handle a WFHttpTask.   

For server, the goal is to fill in the response based on the request.
Similarly, we use an ordinary function to implement process. Read the requested http header one by one and write it into html page.

```cpp
void process(WFHttpTask *server_task)
{
    protocol::HttpRequest *req = server_task->get_req();
    protocol::HttpResponse *resp = server_task->get_resp();
    long seq = server_task->get_task_seq();
    protocol::HttpHeaderCursor cursor(req);
    std::string name;
    std::string value;
    char buf[8192];
    int len;

    /* Set response message body. */
    resp->append_output_body_nocopy("<html>", 6);
    len = snprintf(buf, 8192, "<p>%s %s %s</p>", req->get_method(),
                   req->get_request_uri(), req->get_http_version());
    resp->append_output_body(buf, len);

    while (cursor.next(name, value))
    {
        len = snprintf(buf, 8192, "<p>%s: %s</p>", name.c_str(), value.c_str());
        resp->append_output_body(buf, len);
    }

    resp->append_output_body_nocopy("</html>", 7);

    /* Set status line if you like. */
    resp->set_http_version("HTTP/1.1");
    resp->set_status_code("200");
    resp->set_reason_phrase("OK");

    resp->add_header_pair("Content-Type", "text/html");
    resp->add_header_pair("Server", "Sogou WFHttpServer");
    if (seq == 9) /* no more than 10 requests on the same connection. */
        resp->add_header_pair("Connection", "close");

    // print log
    ...
}
```

Most operations related to HttpMessage have been described in previous documents. The only new operation is append_output_body().   

It is not very efficient to let users generate a complete http body then pass to us. The user are only required to call the append interface, and expand the discrete data into the message block by block.   

The append_output_body() operation will copy the data away, and another interface with nocopy suffix will directly reference the pointer. Be aware not to point to local variables when using it.   

The statement of several related calls can be read in [HttpMessage.h](../src/protocol/HttpMessage.h):

```cpp
class HttpMessage
{
public:
    bool append_output_body(const void *buf, size_t size);
    bool append_output_body_nocopy(const void *buf, size_t size);
    ...
    bool append_output_body(const std::string& buf);
};
```

Again, when you use the append_output_body_nocopy() interface, the life cycle of the data which the buf points at must at least extended to the callback of the task.   

Seq, another variable in the function, is obtained by server_task->get_task_seq(), which indicates the number of the request on the current connection counts from 0.   

In the program, once 10 requests are completed, the connection will be forcibly closed, so:

```cpp
    if (seq == 9) /* no more than 10 requests on the same connection. */
        resp->add_header_pair("Connection", "close");
```

The connection can also be closed through the task->set_keep_alive() interface. For the http protocol, we recommend using the method in which header is set.   

In this example, as the page returned is very small, we paid little attention to whether the reply is successful or not. In the next example http_proxy, we will know how to receive the reply status.
