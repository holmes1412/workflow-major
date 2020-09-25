# About counter

The counter is a very important basic task in the framework. The counter is essentially a semaphore that does not occupy threads.

Counters are mainly used for workflow control. It is divided into two types, i.e.: anonymous counters and named counters, which can implement very complex business logic.

# Creation of counter

Since the counter is also a task, it is also created through WFTaskFactory. There are two creation methods:

~~~cpp
using counter_callback_t = std::function<void (WFCounterTask *)>;

class WFTaskFactory
{
    ...
    static WFCounterTask *create_counter_task(unsigned int target_value,
                                              counter_callback_t callback);
                                              
    static WFCounterTask *create_counter_task(const std::string& counter_name,
                                              unsigned int target_value,
                                              counter_callback_t callback);

    ...
};
~~~

Each counter contains a target_value, when the counter value reaches the target_value, the callback is called.

The above two interfaces generate anonymous counters and named counters respectively, and anonymous counters directly increase the count through the count method of WFCounterTask:

~~~cpp
class WFCounterTask
{
public:
    virtual void count()
    {
        ...
    }
    ...
}
~~~

If you pass in a counter_name when creating a counter, a named counter will be generated, and the count can be increased through the count_by_name function.

# Use anonymous counters to implement task parallelism

In the example of [parallel wget](./tutorial-06-parallel_wget.md), we implement multiple series in parallel by creating a ParallelWork.

Through the combination of ParallelWork and SeriesWork, any serial-parallel graph can be constructed, this is already able to meet the requirements of most application scenarios.

Counters enables us to build more complex task dependencies, for example, to implement a fully connected neural network.

The following simple code can replace ParallelWork to implement a parallel http wget.

~~~cpp
void http_callback(WFHttpTask *task)
{
    /* Save http page. */
    ...

    WFCounterTask *counter = (WFCounterTask *)task->user_data;
    counter->count();
}

std::mutex mutex;
std::condition_variable cond;
bool finished = false;

void counter_callback(WFCounterTask *counter)
{
    mutex.lock();
    finished = true;
    cond.notify_one();
    mutex.unlock();
}

int main(int argc, char *argv[])
{
    WFCounterTask *counter = create_counter_task(url_count, counter_callback);
    WFHttpTask *task;
    std::string url[url_count];

    /* init urls */
    ...

    for (int i = 0; i < url_count; i++)
    {
        task = create_http_task(url[i], http_callback);
        task->user_data = counter;
        task->start();
    }

    counter->start();
    std::unique_lock<std:mutex> lock(mutex);
    while (!finished)   
        cond.wait(lock);
    lock.unlock();
    return 0;
}
~~~

The above program shows how to create a counter with url_count as target value. Count is called once after each http task is completed.

Note that the count times of the anonymous counter cannot exceed the target value, otherwise the counter may have been destroyed by callback and the program behavior is undefined.

The counter->start() call can be placed before the for loop. As long as the counter is created, its count interface can be called, regardless of whether the counter has been started or not.

The count interface call of the anonymous counter can also be written as counter->WFCounterTask::count(). This can be used in the applications highly focusing on performance.

# Server combined with other asynchronous engines

In some cases, our server may need to call the asynchronous client waiting result outside of this framework. The simplest way is that, we just wait synchronously in the process and wake up through condition variables.

The disadvantage is that we occupy a processing thread and turn asynchronous clients of other frameworks into synchronous clients. However, through counter method, we can wait without occupying threads. This method is very simple:

~~~cpp
void some_callback(void *context)
{
    protocol::HttpResponse *resp = get_resp_from_context(context);
    WFCounterTask *counter = get_counter_from_context(context);
    /* write data to resp. */
    ...
    counter->count();
}

void process(WFHttpTask *task)
{
    WFCounterTask *counter = WFTaskFactory::create_counter_task(1, nullptr);

    SomeOtherAsyncClient client(some_callback, context);

    *series_of(task) << counter;
}
~~~

Here, we can take the series where the server task is located as a coroutine, while take the counter with the target value of 1 as a condition variable.

# Named counter

When anonymous counter is in count operation, the pointer of the counter object is directly accessed. This necessarily requires that the number of count call does not exceed the target value during the operation.

But imagine this application scenario when we start 4 tasks at the same time, as long as any 3 tasks are completed, the workflow can continue.

We can use a counter with a target value of 3. After each task is completed, count once, in such case, as long as the three tasks are completed, the counter will be called back.

But this will cause a problem, when the fourth task is completed and counter->count() is called, the counter is already a wild pointer and the program crashes.

We can adopt named counter to solve this problem. It is implemented by naming the counter and counting by name, for example:

~~~cpp
void counter_callback(WFCounterTask *counter)
{
    WFRedisTask *next = WFTaskFactory::create_redis_task(...);
    series_of(counter)->push_back(next);
}

int main(void)
{
    WFHttpTask *tasks[4];
    WFCounterTask *counter;

    counter = WFTaskFactory::create_counter_task("c1", 3, counter_callback);
    counter->start();

    for (int i = 0; i < 4; i++)
    {
        tasks[i] = WFTaskFactory::create_http_task(..., [](WFHttpTask *task){
                                            WFTaskFactory::count_by_name("c1"); });
        tasks[i]->start();
    }

    ...

}
~~~

In this example, 4 concurrent http tasks are called up, 3 of which are completed, and a redis task is started immediately. In actual applications, you may also need to add data transfer code.

In the example, a counter named "c1" is created. In the http callback, the WFTaskFactory::count_by_name() call is used to count.

~~~cpp
class WFTaskFactory
{
    ...
    static void count_by_name(const std::string& counter_name);

    static void count_by_name(const std::string& counter_name, unsigned int n);
    ...
};
~~~

WFTaskFactory::count_by_name method also allows for passing in an integer n, which represents the count value to be increased by this operation. Obviously:

count_by_name("c1") is equivalent to count_by_name("c1", 1).

If the "c1" counter does not exist (not created yet or already completed), then the operation on "c1" produces no effect, so there will be no problem with the anonymous counter pointer.

# Detailed behavior definition of named counter

When WFTaskFactory::count_by_name(name, n) is called:
  * If name does not exist (not created or completed), no behavior.
  * If there is only one counter with the name:
    * If the remaining value of the counter is less than or equal to n, the count is completed, the callback is called, and the counter is destroyed. Process ends.
    * If the remaining value of the counter is larger than n, the count value is increased by n. Process ends.
  * If there are multiple counters with the same name:
    * Take the first counter base on the creation order, supposed its remaining value is m:
      * If the value of m is larger than n, the count is increased by n. Process ends (the remaining value is m-n).
      * If m is smaller than or equal to n, the count is completed, the callback is called, and the first counter is destroyed. Set n=n-m.
        * If n is 0, process ends.
        * If n is larger than 0, take out the next counter with the same name and repeat the entire operation steps.
        
Although the description is very complicated, it can be briefed in a few words. Access all the counters with the name “name” in turn per the order of creation until n is 0.

In other words, count_by_name(name, n) can waken multiple counters at once.

Skillfully using counters helps us implement very complex business logic. In our framework, counters are often used to implement asynchronous locks, or for channels between tasks. It looks more like a control task in forms.

# Extended WFContainerTask of counter

Counter is like a kind of semaphore. As each count operation cannot be accompanied by operation data, this often brings inconvenience.

If you imagine counter as a node on a directed acyclic graph, each count is an incoming edge. It means that, the nodes can have attributes, while the incoming edges do not contain any information.

WFContainerTask is a task that adds attributes to the incoming edge. For relevant definitions refer to [WFContainerTask.h](../src/factory/WFContainerTask.h):

~~~cpp
template<tyename T>
class WFContainerTask : public WFCounterTask
{
public:
    void push(const T& value);
    void push(T&& value);
    ...
};
~~~

Users can view the relevant codes if necessary. Since WFTaskFactory does not provide factory function, to create a container task, users need to call new by themselves.
