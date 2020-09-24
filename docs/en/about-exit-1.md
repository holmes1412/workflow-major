# About exit

Since most of the calls are non-blocking, in the previous example we need to use some mechanism to prevent the main function from early exit.

For example, in the wget example, waiting for user’s Ctrl-C, or parallel_wget which wakens the main thread after all wget ends.

While in several server examples, the stop() operation is blocking, which can ensure all server tasks end normally and the main thread can exit safely.

# Principles for safe exit

In general circumstances, as long as users write programs standardly and imitate the methods described in the example, there will not be much confusion on exit. But here we still need to define the conditions for the program to exit.

  * User cannot call the system exit() function in any callback function such as callback or process, otherwise the behavior is undefined.

  * The condition for main thread to end safely (main function calls exit() or return) is that all tasks have run to callback and no new tasks is called up.

    * All the examples conform to this assumption, waken main function in the callback. This is safe. Don't worry about the situation that the callback has not ended when main returns.
    
    * ParallelWork is a task that also needs to run to callback.
    
    * This rule can be violated under certain circumstances, and the procedural behavior is strictly defined. But those who do not understand the core principles shall follow this rule, otherwise the program cannot exit normally.

  * All servers must stop to complete, otherwise the behavior is undefined. Since all users can operate stop, so general server program will not have any exit problems.
  
As long as the above conditions are met, the program can exit normally without any memory leaks. Although the definition is very strict, one thing we should note is the condition for the completion of server stop.

  * The server's stop() call will wait till the end of all server task callbacks (this callback is void by default), and no new server tasks will be processed.

  * However, if the user starts a new task in the process and is not in the series where the server task is located, the framework cannot stop and server stop cannot wait till the task is completed.

  * Similarly, if the user adds a new task (such as logging) to the series where the task is located in the callback of the server task, the new task will not be controlled by the server.

  * In the above two cases, if main function exits immediately after server.stop(), then the second rule above may be violated. Because there are still tasks that have not run to callback.

In view of the above situation, the user needs to ensure that the started task has run to callback. The approach can use a counter to record how many tasks are running, and wait till this number is zeroed before main returns.

In the following example, in the callback of server task, a logging file writing task is added to the current series (suppose the file writing is very slow and an asynchronous IO needs to be started):

~~~cpp
std::mutex mutex;
std::condition_variable cond;
int log_task_cnt = 0;

void log_callback(WFFileIOTask *log_task)
{
    mutex.lock();
    if (--log_task_cnt == 0)
        cond.notify_one();
    mutex.unlock();
}

void reply_callback(WFHttpTask *server_task)
{
    WFFileIOTask *log_task = WFTaskFactory::create_pwrite_task(..., log_callback);

    mutex.lock();
    log_task_cnt++;
    mutex.unlock();
    *series_of(server_task) << log_task;
}

int main(void)
{
    WFHttpServer server;

    server.start();
    pause();
    ...

    server.stop();

    std::unique_lock<std::mutex> lock(mutex);
    while (log_task_cnt != 0)
        cond.wait(lock);
    lock.unlock();
    return 0;
}
~~~

Although the above method is feasible, it does increase the complexity of the program and the risk of error, so it should be avoided if possible. 

For example, you can write log directly in reply callback.

# Memory leak occurs when exit OpenSSL 1.1

We found that some openssl1.1 versions have the problem of incomplete memory release when exiting. The memory leak can be detected using valgrind memory check tool.

This problem only occurs when the user uses SSL, for example, crawls https webpage. In general, users can ignore such leak. If you really want to solve the problem, use the method below:

~~~cpp
#include <openssl/ssl.h>

int main()
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(0, NULL);
#endif
    ...
}
~~~

That is to say, before using the library, first initialize openssl. Configure openssl parameters at the same time if necessary.

Note that this function is only available in openssl1.1 version or above, so you need to check the openssl version before calling.

The memory leak is related to the memory release principle of openssl1.1. The solution we provide can solve this problem (but we still recommend users to ignore it).
