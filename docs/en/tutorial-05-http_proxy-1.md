# Example of asynchronous server: http_proxy

# Sample code

[tutorial-05-http\_proxy.cc](../tutorial/tutorial-05-http_proxy.cc)

# About http_proxy

This is an http proxy server, which can be configured and used in the browser. It supports all http methods.   

Because the principle of https proxy is different, this example does not support https proxy, so you can only browse http websites.

In the implementation process, this proxy needs to crawl the complete http page before forwarding it out, and there will be a delay when downloading/uploading large files.

# Change server configurations

In the previous example, we used the default http server parameters. While in this example, we make a little change to limit the size of the request to prevent malicious attacks.

```cpp
int main(int argc, char *argv[])
{
    ...
    struct WFServerParams params = HTTP_SERVER_PARAMS_DEFAULT;
    params.request_size_limit = 8 * 1024 * 1024;

    WFHttpServer server(&params, process);
    if (server.start(port) == 0)
    {
        pause();
        server.stop();
    }
    else
    {
        perror("cannot start server");
        exit(1);
    }

    return 0;   
}
```

Different from the previous example, we construct in the server and pass in one parameter structure. We can take a look at the http server configurations.   

In [WFHttpServer.h](../src/server/WFHttpServer.h), the default parameters of http server are as below:   

```cpp
static constexpr struct WFServerParams HTTP_SERVER_PARAMS_DEFAULT =
{
    .max_connections        =    2000,
    .peer_response_timeout  =    10 * 1000,
    .receive_timeout        =    -1,
    .keep_alive_timeout     =    60 * 1000,
    .request_size_limit     =    (size_t)-1,
    .ssl_accept_timeout     =    10 * 1000,
};
```

max_connections: The maximum number of connections is 2000. Once reaches the upper limit, the keep-alive connection that has not been used for the longest time will be closed. If keep-alive connection is not found, any new connection will be refused.   
peer_response_timeout: The timeout period for each piece of data read or sent is 10 seconds.   
receive_timeout: The timeout period for receiving a complete request is -1, unlimited.   
keep_alive_timeout: The connection is kept for 1 minute.   
request_size_limit: The maximum size of the request packet, unlimited.   
ssl_accept_timeout: 10 seconds to complete the ssl handshake timeout.   

There is no send_timeout, namely complete reply timeout in the parameters. This parameter needs to be determined according to the size of the reply packet for each request.   

# Business logic of proxy server

This proxy server essentially forwards the user's request to the corresponding web server intact, and then forwards the web server's reply to the user intact. In the request that the browser sent to proxy, the request uri contains the scheme, host, and port, which need to be removed before forwarding.   

For example, when you visit http://www.sogou.com/, the first line of the request that the browser sent to proxy is:   

GET http://www.sogou.com/ HTTP/1.1, which needs to be rewritten as:   

GET / HTTP/1.1

```cpp
void process(WFHttpTask *proxy_task)
{
    auto *req = proxy_task->get_req();
    SeriesWork *series = series_of(proxy_task);
    WFHttpTask *http_task; /* for requesting remote webserver. */

    tutorial_series_context *context = new tutorial_series_context;
    context->url = req->get_request_uri();
    context->proxy_task = proxy_task;

    series->set_context(context);
    series->set_callback([](const SeriesWork *series) {
        delete (tutorial_series_context *)series->get_context();
    });

    http_task = WFTaskFactory::create_http_task(req->get_request_uri(), 0, 0,
                                                http_callback);

    const void *body;
    size_t len;

    /* Copy user's request to the new task's reuqest using std::move() */
    req->set_request_uri(http_task->get_req()->get_request_uri());
    req->get_parsed_body(&body, &len);
    req->append_output_body_nocopy(body, len);
    *http_task->get_req() = std::move(*req);

    /* also, limit the remote webserver response size. */
    http_task->get_resp()->set_size_limit(200 * 1024 * 1024);

    *series << http_task;
}
```

That is the entire content of the process. First analyze the structure of the http request sent to web server.   

The req->get_request_uri() call gets the complete URL requested by the browser, and constructs the http task sent to the server through this URL.   

The number of both retries and redirects of this http task is 0, because the redirect is handled by the browser, and the request will be reissued when it encounters 302.   

```cpp
    req->set_request_uri(http_task->get_req()->get_request_uri());
    req->get_parsed_body(&body, &len);
    req->append_output_body_nocopy(body, len);
    *http_task->get_req() = std::move(*req);
```

The above four statements are actually generating http requests sent to web server. req is the http request we received, and we will eventually move it directly to the new request through std::move().   

The first line actually removes the http://host:port part of request_uri, and only retains the part after the path.   

The second and third lines specify the parsed http body as the output http body. The reason for this operation is that in the implementation of HttpMessage, the body obtained by parsing and the body of sending request are two domains, so what we need do is simply setting the configures, with no need to copy memory.   

The fourth line transfers the request content to the request sent to web server at one time. After constructing http request, put the request to the end of the current series, and complete the process function.   

# Working principles of asynchronous server

Obviously the process function doesn’t represent the whole proxy logic, we also need to process the http response returned from web server and fill in the response returned to the browser.   

In the example of echo server, we don't need network communication, instead we just need to fill in the return page directly. In case of proxy, we need to wait for the result of web server.   

Of course we can occupy the thread of the process function and wait for the result to return, but this is obviously not the best option.   

Then, we need to reply to the user request after the request result is asynchronously obtained, and no thread can be occupied while we are waiting for the result.   

Therefore, in the head of the process, we set a context for the current series, which contains the proxy_task so that we can fill in the results asynchronously.   

```cpp
struct tutorial_series_context
{
    std::string url;
    WFHttpTask *proxy_task;
    bool is_keep_alive;
};

void process(WFHttpTask *proxy_task)
{
    SeriesWork *series = series_of(proxy_task);
    ...
    tutorial_series_context *context = new tutorial_series_context;
    context->url = req->get_request_uri();
    context->proxy_task = proxy_task;

    series->set_context(context);
    series->set_callback([](const SeriesWork *series) {
        delete (tutorial_series_context *)series->get_context();
    });
    ...
}
```

As noted in the previous client example, any running task is in a series, so is server task.   

Therefore, we can get the current series and set the context. The url is mainly used for subsequent logging, proxy_task is the main content, and resp needs to be filled in as a part of later operations.   

Next, we can refer to how to process web server response.   

```cpp
void http_callback(WFHttpTask *task)
{
    int state = task->get_state();
    auto *resp = task->get_resp();
    SeriesWork *series = series_of(task);
    tutorial_series_context *context =
        (tutorial_series_context *)series->get_context();
    auto *proxy_resp = context->proxy_task->get_resp();

    ...
    if (state == WFT_STATE_SUCCESS)
    {
        const void *body;
        size_t len;

        /* set a callback for getting reply status. */
        context->proxy_task->set_callback(reply_callback);

        /* Copy the remote webserver's response, to proxy response. */
        if (resp->get_parsed_body(&body, &len))
            resp->append_output_body_nocopy(body, len);
        *proxy_resp = std::move(*resp);
        ...
    }
    else
    {
        // return a "404 Not found" page
        ...
    }
}
```

We only focus on the success/failure status. As long as a complete http page from web server can be received, it is considered a success no matter what the return code is. A return on the 404 page can be considered a failure case. Because the data returned to the user can be huge, in our example, the upper limit is set to 200MB. For this reason, unlike the previous example, we need to check the success/failure status of the reply.   

The http server task and the http client task we created ourselves are exactly in the same type, both are WFHttpTask. The difference is that server task is created by the framework, and its callback is initially void.   

The callback of the server task, like the callback of the client, is called after the http interaction is completed. Therefore, for the server task, it is called after the reply is completed.   

We might be very familiar with the next three lines of code. Transfer web server response packet to proxy response packet without copying.   

After the http_callback function finishes, the reply to browser is sent out, and everything takes place in an asynchronous process.   

The remaining function is reply_callback(), here it just aims to print some logs. After this callback is executed, proxy task will be automatically deleted.
Finally, destruction context in callback series.   

# Timing of server reply 

It’s worth noting that, the message will not be replied until all other tasks in the series are executed, and the reply is automatic, so there is no task->reply() interface.   

However, there is a task->noreply() call. If this call is executed on the server task, the connection is directly closed at the time of the original reply. But the callback will still be called (status as NOREPLY).   

In the callback of server task, the series of the task can also be obtained through the series_of() operation. That is to say, we can still add new tasks to this series, although the reply has been completed.   

If users need to continue to add tasks to the series, please refer to the instructions [on program exit](./about-exit.md). This may cause the problem of unfinished tasks left after the server is shut down.   
