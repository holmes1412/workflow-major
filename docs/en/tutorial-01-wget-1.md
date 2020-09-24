# Create your first task: wget

# Sample code

[tutorial-01-wget.cc](../../tutorial/tutorial-01-wget.cc)

# About wget

The program reads the http/https URL from stdin, crawls the web page and prints the content in stdout. The request and response of http headers shall print in stderr.

For the sake of simplicity, the program shall exit with Ctrl-C, and ensure all resources are completely released in advance.

# Create and start http task

~~~cpp
WFHttpTask *task = WFTaskFactory::create_http_task(url, REDIRECT_MAX, RETRY_MAX, wget_callback);
protocol::HttpRequest *req = task->get_req();
req->add_header_pair("Accept", "*/*");
req->add_header_pair("User-Agent", "Wget/1.14 (gnu-linux)");
req->add_header_pair("Connection", "close");
task->start();
pause();
~~~

WFTaskFactory::create_http_task() creates a http task, in [WFTaskFactory.h](../../src/factory/WFTaskFactory.h) file, the prototype is defined as below:

~~~cpp
WFHttpTask *create_http_task(const std::string& url,
                             int redirect_max, int retry_max,
                             http_callback_t callback);
~~~

Without having to explain the previous parameters, http_callback_t is the callback of http task, which defined as below:

~~~cpp
using http_callback_t = std::function<void (WFHttpTask *)>;
~~~

To put it bluntly, it is a function whose parameter is the Task itself and without return value. This callback can transfer NULL, which means no callback is required and works the same for all callbacks in our tasks.

Please noted that all factory functions will not return failure, so don't worry if the task shall become NULL, even if the URL is illegal. All errors will handled again in the callback.

The task->get_req() function gets the request of the task with GET method HTTP/1.1 and long connection by default. The framework will automatically add request_uri, Host, etc., and will add http headers such as Content-Length or Connection as needed before sending. Users should also add their own headers through add_header_pair() method. More interfaces on http messages can be viewed in [HttpMessage.h](../../src/protocol/HttpMessage.h).

task->start() to start the task, non-blocking, and will not fail. Then the callback must be invoked again. For asynchronous reasons, it is obvious that the task pointer cannot be used after start().

In order to make the example as simple as possible, invoke pause() after start() to prevent program exit, the user needs to end the program with Ctrl-C.

# Process http crawl results

In this example, we use a general function to process the result. Of course, std::function can support more functions.

~~~cpp
void wget_callback(WFHttpTask *task)
{
    protocol::HttpRequest *req = task->get_req();
    protocol::HttpResponse *resp = task->get_resp();
    int state = task->get_state();
    int error = task->get_error();

    // handle error states
    ...

    std::string name;
    std::string value;
    // print request to stderr
    fprintf(stderr, "%s %s %s\r\n", req->get_method(), req->get_http_version(), req->get_request_uri());
    protocol::HttpHeaderCursor req_cursor(req);
    while (req_cursor.next(name, value))
        fprintf(stderr, "%s: %s\r\n", name.c_str(), value.c_str());
    fprintf(stderr, "\r\n");
    
    // print response header to stderr
    ...

    // print response body to stdin
    void *body;
    size_t body_len;
    resp->get_parsed_body(&body, &body_len); // always success.
    fwrite(body, 1, body_len, stdout);
    fflush(stdout);
}
~~~

In this callback, the task is the task we generated through the factory. 

task->get_state() and task->get_error() obtain the running status and error code of the task respectively. We shall ignore the error handled session first.

task->get_resp() gets the response of the task by deriving from HttpMessage, which is the same with request .

Then scan the headers of request and response through HttpHeaderCursor object. Find the definition of Cursor at [HttpUtil.h](../../src/protocol/HttpUtil.h).

~~~cpp
class HttpHeaderCursor
{
public:
    HttpHeaderCursor(const HttpMessage *message);
    ...
    void rewind();
    ...
    bool next(std::string& name, std::string& value);
    bool find(const std::string& name, std::string& value);
    ...
};
~~~

There shall be no doubts for the cursor usage.

The next line resp->get_parsed_body() gets the http body of the response. This invoke will return to true once the task is successful, and the body points to the data zone.

The invoke results to the original http body and without decoding the chunk encoding. To get decoded chunk encoding, use HttpChunkCursor in [HttpUtil.h](../../src/protocol/HttpUtil.h).

In addition, it should be noted that the find() interface will modify the pointer inside the cursor, that is, if you still want to traverse the header after using find(), return to the cursor head through the rewind() interface is required.
