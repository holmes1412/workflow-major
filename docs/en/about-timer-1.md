# About timer

The function of the timer is to wait for a definite time without occupy threads. Similarly, the timer expiry is notified through callback.

# Creation of timer

It follows the same method as that of WFTaskFactory class:

~~~cpp
using timer_callback_t = std::function<void (WFTimerTask *)>;

class WFTaskFactory
{
    ...
    static WFTimerTask *create_timer_task(unsigned int microseconds,
                                          timer_callback_t callback);
};
~~~

The first parameter is the timer time in microseconds. Except for program exit, the timer cannot end prematurely.

Likewise, there is also a user_data field in the timer task that can be used to transfer some user data. Start method and accessing the task flow are the same as that in other tasks.

# An advanced feature of timer

As noted in [About exit](./about-exit.md), when the main function ends or exit() is called, all tasks must run to the callback, and no new tasks are called.

This may cause a problem. The longest time of timer exceeds 1 hour and cannot be interrupted actively. If you wait till the timer expires, it will take a long time to exit the program.

In implementation practice, the timer can be interrupted by program exit, allowing the timer to return to callback. If the timer is interrupted by program exit, get_state() will get a WFT_STATE_ABORTED state.

Of course, if the timer is interrupted by program exit, no more new tasks can be called up.

The following program crawls a http page at interval of a second. When all the urls are grabbed, the program will exits directly without waiting for the timer to return to the callback, and no delay will occur in program exit.

~~~cpp
bool program_terminate = false;

void timer_callback(WFTimerTask *timer)
{
    mutex.lock();
    if (!program_terminate)
    {
        WFHttpTask *task;
        if (urls_to_fetch > 0)
        {
            task = WFTaskFactory::create_http_task(...);
            series_of(timer)->push_back(task);
        }

        series_of(timer)->push_back(WFTaskFactory::create_timer_task(1000000, timer_callback));
    }
    mutex.unlock();
}

...
int main()
{
    ....
    /* all urls done */
    mutex.lock();
    program_terminate = true;
    mutex.unlock();
    return 0;
}
~~~

In the above program, timer_callback must determine the program_terminate condition in the lock, otherwise a new task may be called after the program has ended. As it has difficulties in the usage, the program should try to avoid using this feature, and should not end the program until all timers return to the callback.

# What if timer time is not sufficient

At present, the maximum timer duration is about 4200 seconds. If the task of the program is started once every 24 hours, a 24-hour timer is required. You can simply add multiple timers.

For example:

~~~cpp
void timer_callback(WFTimerTask *timer)
{
    mutex.lock();
    if (program_terminate)
        series_of(timer)->cancel();
    mutex.unlock();
}

void my_callback(WFMyTask *task)
{
    SeriesWork *series = series_of(task);
    WFTimerTask *timer;
    for (int i = 0; i < 24; i++)
    {
        timer = WFTaskFactory::create_timer_task(3600U*1000*1000, timer_callback);
        series->push_back(timer);
    }

    WFMyTask *next_task = MyFactory::create_my_task(..., my_callback);
    series->push_back(next_task);
}
~~~

Because timer_task is a task that consumes very little resources, so you can create many timers. In the above example, 24 1-hour timers are created, and a task is executed every 24 hours.

The example also considers the problem that the program can exit at any time. It is found in the callback of the timer that the remaining tasks need to be cancelled after program exit.

Although the timer can be interrupted by program exit, we also support multiple timers in series to achieve a rather long timer time, which is not a recommended practice. In most cases, you should avoid long timer time, and should not exit the program until all timers expire.
