# More features about task series: wget_to_redis

# Sample code

[tutorial-03-wget\_to\_redis.cc](../tutorial/tutorial-03-wget_to_redis.cc)

# About wget_to_redis

The program reads a http URL and a redis URL from the command line, and saves the crawled Http page (with the http URL as the key) in redis.   

Different from the previous two examples, we added a wakening mechanism to allow the program to exit automatically, with no need to use Ctrl-C.   

# Create Http task and set parameters

Similar to the previous example, this example also executes two requests in serial. The major difference is we need to notify the main thread task already ends and exits normally.   

We add two more calls to limit the size of the crawled and returned http content, and the maximum time to receive a reply.   

```cpp
WFHttpTask *http_task = WFTaskFactory::create_http_task(...);
...
http_task->get_resp()->set_size_limit(20 * 1024 * 1024);
http_task->set_receive_timeout(30 * 1000);
```

set_size_limit() is a call of HttpMessage to limit the packet size when receiving http messages. In fact, all protocol messages require this interface.

set_receive_timeout() is the timeout of receiving data in ms.

The above code limits the http message to to 20M, and the total reception time is limited to 30 seconds. We have more detailed timeout configurations, which will be described later.

# Create and start SeriesWork

In the previous two sets of examples, we directly call task->start() to start the first task. The working procedure for task->start() operation is,

First create a SeriesWork with task as the first task, and then start this series. You can see the implementation of start in [WFTask.h](../src/factory/WFTask.h):

```cpp
template<class REQ, class RESP>
class WFNetWorkTask : public CommRequest
{
public:
    void start()
    {
        assert(!series_of(this));
        Workflow::start_series_work(this, nullptr);
    }
    ...
};
```

We want to set a callback to the series and add some context. So we create a series by our own, instead of using the start interface of the task.   

SeriesWork cannot be new, deleted, nor derived. It can only be generated through the Workflow::create_series_work() interface. In [Workflow.h](../src/factory/Workflow.h), the common practice is to use the below call:

```cpp
using series_callback_t = std::function<void (const SeriesWork *)>;

class Workflow
{
public:
    static SeriesWork *create_series_work(SubTask *first, series_callback_t callback);
};
```

In the sample code, the common practice is:

```cpp
struct tutorial_series_context
{
    std::string http_url;
    std::string redis_url;
    size_t body_len;
    bool success;
};
...
struct tutorial_series_context context;
...
SeriesWork *series = Workflow::create_series_work(http_task, series_callback);
series->set_context(&context);
series->start();
```

In the previous example, we used the void *user_data pointer in the task to save the context information. But in this example, we put the context information in the series. This is obviously more reasonable since series is a complete task chain, and all tasks can be obtained and modified in the context.   

The callback function of series is called after all tasks in the series are executed. We simply use a lamda function to print the running result and waken the main thread.

# The rest of work
Start a redis task written library once successfully crawling http, which is common practice. If fails to crawl or the http body length is 0, then redis task will not be started.   

In any occassion, the program can exit normally after all tasks complete, because all the tasks are in the same series.
