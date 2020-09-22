# A simple parallel wget: parallel_wget

# Sample code

[tutorial-06-parallel\_wget.cc](../tutorial/tutorial-06-parallel_wget.cc)

# About parallel_wget

This is the first example of a parallel task.   

The program reads multiple http URLs (separated by spaces) from the command line, crawls these URLs in parallel, and prints the wget results to standard output in the input order.

# Create parallel task

In previous examples, we have already get to know SeriesWork.

- SeriesWork is composed of tasks, representing the serial execution of a series of tasks. When all tasks are over, the series ends.
- ParallelWork is in corresponding to SeriesWork, which composed of series, representing the parallel execution of several series. When all series ends, this parallel ends.
- ParallelWork is a kind of task.

According to the above definition, we can dynamically or statically generate any complex workflow.

In the Workflow class, there are two interfaces used for generating parallel tasks:

```cpp
class Workflow
{
    ...
public:
    static ParallelWork *
    create_parallel_work(parallel_callback_t callback);

    static ParallelWork *
    create_parallel_work(SeriesWork *const all_series[], size_t n,
                         parallel_callback_t callback);

    ...
};
```

The first interface creates a void parallel task, and the second interface creates a parallel task with a series array.   

No matter which interface is used to generate parallel tasks, you can use the add_series() interface of ParallelWork to add series before start.   

In the example code, we create a void parallel task and add series one by one.   

```cpp
int main(int argc, char *argv[])
{
    ParallelWork *pwork = Workflow::create_parallel_work(callback);
    SeriesWork *series;
    WFHttpTask *task;
    HttpRequest *req;
    tutorial_series_context *ctx;
    int i;

    for (i = 1; i < argc; i++)
    {
        std::string url(argv[i]);
        ...
        task = WFTaskFactory::create_http_task(url, REDIRECT_MAX, RETRY_MAX,
            [](WFHttpTask *task)
        {
            // store resp to ctx.
        });

        req = task->get_req();
        // add some headers.
        ...

        ctx = new tutorial_series_context;
        ctx->url = std::move(url);
        series = Workflow::create_series_work(task, nullptr);
        series->set_context(ctx);
        pwork->add_series(series);
    }
    ...
}
```

As you can see from the code, we create http task first, but http task cannot be directly added to the parallel task. You need to use it to create a series first.   

Each series has a context, which is used to save URL and wget results. We have described related methods in the previous examples.   

# Save and use wget result

The callback of http task is a simple lambda function, which saves the wget result in its own series context to be accessible by parallel task.   

```cpp
    task = WFTaskFactory::create_http_task(url, REDIRECT_MAX, RETRY_MAX,
        [](WFHttpTask *task)
    {
        tutorial_series_context *ctx =
            (tutorial_series_context *)series_of(task)->get_context();
        ctx->state = task->get_state();
        ctx->error = task->get_error();
        ctx->resp = std::move(*task->get_resp());
    });
```

It is essential to do so because http task will be recycled after callback. We can only remove the resp through std::move() operation.   

In the callback of the parallel task, we can easily get the result:

```cpp
void callback(const ParallelWork *pwork)
{
    tutorial_series_context *ctx;
    const void *body;
    size_t size;
    size_t i;

    for (i = 0; i < pwork->size(); i++)
    {
        ctx = (tutorial_series_context *)pwork->series_at(i)->get_context();
        printf("%s\n", ctx->url.c_str());
        if (ctx->state == WFT_STATE_SUCCESS)
        {
            ctx->resp.get_parsed_body(&body, &size);
            printf("%zu%s\n", size, ctx->resp.is_chunked() ? " chunked" : "");
            fwrite(body, 1, size, stdout);
            printf("\n");
        }
        else
            printf("ERROR! state = %d, error = %d\n", ctx->state, ctx->error);

        delete ctx;
    }
}
```

Here, we see two new interfaces of ParallelWork, i.e.: size() and series_at(i), to obtain the number of parallel series and the i-th parallel series respectively.

Get the context of the corresponding series through series->get_context() and print the result. The printing order must be consistent with the order which we put it in.   

In this example, there is no other work after the parallel task is executed.

As we mentioned above, ParallelWork is a kind of task. So we can also use series_of() to get the series it is in and add new tasks.

However, if the new task still needs to use the wget result, we need to use std::move() again to migrate the data to the context of the series where the parallel task is located.

# Start a parallel task

As parallel task is a kind of task, so you can call start() directly, and use it to create or start a series. There’s nothing particular to mention for the start of parallel task.   

In this example, we start a series, waken the main process in the callback of this series, and exit the program normally.   

We can also waken the main process in the callback of parallel task, and this is not quite difference in program behavior. But wakening in series callback is more standard.
