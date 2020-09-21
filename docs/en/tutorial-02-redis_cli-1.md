# Implement write and read of redis: redis_cli

# Sample code

[tutorial-02-redis\_cli.cc](../tutorial/tutorial-02-redis_cli.cc)

# About redis\_cli

The program reads a redis server address and a pair of key, value from the command line. Execute the SET command to write the pair of KVs, and then read it to verify whether the write is successful.

Program running method: ./redis_cli <redis URL><key><value>

For the sake of simplicity, the program needs to be ended with Ctrl-C.

# Redis URL format
redis://:password@host:port/dbnum?query#fragment   
In case of SSL, then:
rediss://:password@host:port/dbnum?query#fragment   
The password is optional. The default value of port is 6379, the default value of dbnum is 0, within the range of 0-15.   
The query and fragment parts are not explained in some factories, and users can define by themselves. For example, if users have upstream selection requirements, they can customize query and fragment. Please refer to the upstream document for related information.   
Example of redis URL:   
redis://127.0.0.1/   
redis://:12345678@redis.some-host.com/1   

# Create and start Redis task
There is little difference between creating a Redis task and creating an http task, besides the absence of redirect_max parameter.

```cpp
using redis_callback_t = std::function<void (WFRedisTask *)>;

WFRedisTask *create_redis_task(const std::string& url,
                               int retry_max,
                               redis_callback_t callback);
```

In this example, we want to storage user information, including url and key, in the redis task. Thus such informatio can be used in the callback.

Of course, we can use std::function to bound parameters. Here we use the void *user_data pointer in the task, as a public member.

```
struct tutorial_task_data
{
    std::sring url;
    std::string key;
};
...
struct tutorial_task_data data;
data.url = argv[1];
data.key = argv[2];

WFRedisTask *task = WFTaskFactory::create_redis_task(data.url, RETRY_MAX, redis_callback);

protocol::RedisRequest *req = task->get_req();
req->set_request("SET", { data.key, argv[3] });

task->user_data = &data;
task->start();
pause();
```

Similar to get_req() in http task, get_req() return task of redis task corresponds to redis request.

The functions provided by RedisRequest can be viewed in [RedisMessage.h](../src/protocol/RedisMessage.h). Among all, set_request interface is used to set redis command.

```
void set_request(const std::string& command, const std::vector<std::string>& params);
```

We believe those who often use redis will not have any questions on this interface. But it must be noted that our request is to disable both SELECT and AUTH command.   
As the user cannot specify a specific connection for each request, it’s not guaranteed that the next request after the SELECT will be started on the same connection. So this command makes no sense to the user.   

Please specify the database selection and password in the redis URL. In addition, each requested URL must be accompanied by this information.   

# Process request result
Complete the SET command and start a GET command to verify the writing result. The GET command also shares the same callback. Therefore, the function will decide which command the result belongs to.   
Likewise, we shall ignore the incorrect processed session.   

```cpp
void redis_callback(WFRedisTask *task)
{
    protocol::RedisRequest *req = task->get_req();
    protocol::RedisResponse *resp = task->get_resp();
    int state = task->get_state();
    int error = task->get_error();
    protocol::RedisValue val;

    ...
    resp->get_result(val);
    std::string cmd;
    req->get_command(cmd);
    if (cmd == "SET")
    {
        tutorial_task_data *data = (tutorial_task_data *)task->user_data;
        WFRedisTask *next = WFTaskFactory::create_redis_task(data->url, RETRY_MAX, redis_callback);

        next->get_req()->set_request("GET", { data->key });
        series_of(task)->push_back(next);
        fprintf(stderr, "Redis SET request success. Trying to GET...\n");
    }
    else /* if (cmd == 'GET') */
    {
        // print the GET result
        ...
        fprintf(stderr, "Finished. Press Ctrl-C to exit.\n");
    }
}
```

RedisValue is the result of a redis request, and its interface can be found at [RedisMessage.h](../src/protocol/RedisMessage.h).   

Since it is the first time to use Workflow's function, the part requires special notice in Callback is series_of(task)->push_back(next)..   

Then we will start next in the redis task, and execute GET operation. Please do not execute next->start() to start the task, instead you shall push_back the next task to the end of the current task sequence.   

The differences in between lie in:   

* If use start to start the task, the task will be started immediately; if use the push_back method, the next task will not be started until the callback completes.
** Link construction rationalize has profit inside to SEO. Push_back method can ensure the order inside log printing. Otherwise, in next->start() method, the "Finished." log in the exmple may be printed first.
* If use start to start the next task, the current task series will finish, and the next task will start a new series.
** Though it is not shown in the example, series can set callback.
** In a parallel task, series is a branch of the parallel task. The branch will be considered complete once the series finished. Parallel task will be further introduced in this tutorials.   

In short, if you want to start the next task when previous task complete, use push_back to complete the operation (push_front may be used in some occassions).

Series_of() is a very important call, and it is a global function that does not belong to any category. Visit Workflow.h to find its definition and implementation method.   

```cpp
static inline SeriesWork *series_of(const SubTask *task)
{
    return (SeriesWork *)task->get_pointer();
}
```

All tasks are the derivative of the SubTask type. And any running task must belong to a certain series. Through the call of series_of, we can obtain the series that the task belongs to.

Push_back is a call of the SeriesWork, with the function to put a task at the end of the series. Push_front is another call of this kind. In this example, it makes no difference for whichever call is used.

```cpp
class SeriesWork
{
    ...
public:
    void push_back(SubTask *task);
    void push_front(SubTask *task);
    ...
}
```

SeriesWork plays an important role in the entire system. In the next example, we will present you more features of SeriesWork.
