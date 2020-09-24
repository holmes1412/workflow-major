# Asynchronous IO http server: http_file_server

# Sample code

[tutorial-09-http_file_server.cc](../../tutorial/tutorial-09-http_file_server.cc)

# About http_file_server

Http_file_server is a web server, the user can start a web server by specifying the start port and root path (the program is the route by default).

Users can also specify a certificate file and key file in PEM format to start an https web server.

The program mainly shows the usage of disk IO tasks. In Linux system, we use the aio interface at the bottom of Linux, and file reading is completely asynchronous.

# Start server

About the start of server, it has nothing different from that of previous echo server or http proxy, except for one more way to start SSL server:

~~~cpp
class WFServerBase
{
    ...
    int start(unsigned short port, const char *cert_file, const char *key_file);
    ...
};
~~~

In other words, you can specify a cert file and key file in PEM format to start an SSL server.

In addition, when we define the server, we use std::bind() to bind a root parameter to the process, which represents the root path of the service.

~~~cpp
void process(WFHttpTask *server_task, const char *root)
{
    ...
}

int main(int argc, char *argv[])
{
    ...
    const char *root = (argc >= 3 ? argv[2] : ".");
    auto&& proc = std::bind(process, std::placeholders::_1, root);
    WFHttpServer server(proc);

    // start server
    ...
}
~~~

# Process request

Similar to http_proxy, we do not occupy any thread to read the file, but generate an asynchronous file reading task, and reply to the request after the reading is completed.

Again, we need to read the complete reply data into the memory before we start replying to the message. For this reason, it is not suitable to transfer large files.

~~~cpp
void process(WFHttpTask *server_task, const char *root)
{
    // generate abs path.
    ...

    int fd = open(abs_path.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        size_t size = lseek(fd, 0, SEEK_END);
        void *buf = malloc(size);        /* As an example, assert(buf != NULL); */
        WFFileIOTask *pread_task;

        pread_task = WFTaskFactory::create_pread_task(fd, buf, size, 0,
                                                      pread_callback);
        /* To implement a more complicated server, please use series' context
         * instead of tasks' user_data to pass/store internal data. */
        pread_task->user_data = resp;    /* pass resp pointer to pread task. */
        server_task->user_data = buf;    /* to free() in callback() */
        server_task->set_callback([](WFHttpTask *t){ free(t->user_data); });
        series_of(server_task)->push_back(pread_task);
    }
    else
    {
        resp->set_status_code("404");
        resp->append_output_body("<html>404 Not Found.</html>");
    }
}
~~~

Unlike http_proxy which generates a new http client task, here we generate a pread task through factory.

For details of related interface, please refer to [WFTaskFactory.h](../src/factory/WFTaskFactory.h). 

~~~cpp
struct FileIOArgs
{
    int fd;
    void *buf;
    size_t count;
    off_t offset;
};

...
using WFFileIOTask = WFFileTask<struct FileIOArgs>;
using fio_callback_t = std::function<void (WFFileIOTask *)>;
...

class WFTaskFactory
{
public:
    ...
    static WFFileIOTask *create_pread_task(int fd, void *buf, size_t count, off_t offset,
                                           fio_callback_t callback);

    static WFFileIOTask *create_pwrite_task(int fd, void *buf, size_t count, off_t offset,
                                            fio_callback_t callback);
    ...
};
~~~

Whether it is pread or pwrite, both return to WFFileIOTask. This is the same like not distinguishing sort or psort, client or server task.

In addition to these two interfaces, there are preadv and pwritev, which return WFFileVIOTask, fsync and fdsync, then return WFFileSyncTask. They can be found in the header file.

At present, this set of interface requires users to open and close fd by themselves. We are developing a set of file management. In the future, users only need to input the file name, which is more cross-platform friendly.

The example uses the user_data domain of the task to save global data of the service. But for large services, we recommend using series context. You can refer to the previous [proxy example](../tutorial/tutorial-05-http_proxy.cc).

# Process read file result

~~~cpp
using namespace protocol;

void pread_callback(WFFileIOTask *task)
{
    FileIOArgs *args = task->get_args();
    long ret = task->get_retval();
    HttpResponse *resp = (HttpResponse *)task->user_data;

    close(args->fd);
    if (ret < 0)
    {
        resp->set_status_code("503");
        resp->append_output_body("<html>503 Internal Server Error.</html>");
    }
    else /* Use '_nocopy' carefully. */
        resp->append_output_body_nocopy(args->buf, ret);
}
~~~

get_args() of file task gets input parameters, which is the FileIOArgs structure.

get_retval() is the return value of the operation. When ret <0, the task is wrong. Otherwise, ret is the value of the data read.

In a file task, ret <0 is completely equivalent to task->get_state() != WFT_STATE_SUCCESS.

The memory of buf domain is managed by ourselves, and it can be passed to resp through append_output_body_nocopy().

After the reply is completed, we will free() this memory. This statement is in the process:

server_task->set_callback([](WFHttpTask *t){ free(t->user_data); });

# About the implementation of file asynchronous IO

Linux operating system supports a set of asynchronous IO system calls with high efficiency and very low CPU usage. If our framework is used under Linux system, this set of interfaces will be used by default.

We have implemented a set of posixaio interface to support other UNIX systems, and used the threaded sigevent notification method, but because of its low efficiency, it is no longer used.

At present, for non-Linux systems, asynchronous IO is always implemented by multi-threading. When an IO task arrives, a thread is created in real time to execute the IO task, and callback returns to the handler thread pool.

Multi-threaded IO is also the only option under macOS, because macOS does not have good sigevent support, and posixaio does not work.

Multi-threaded IO does not support preadv and pwritev tasks. If you create and run these two tasks, you will get an ENOSYS error in the callback.

Some UNIX systems do not support fdatasync calls. In this case, fdsync task will be equivalent to fsync task.
