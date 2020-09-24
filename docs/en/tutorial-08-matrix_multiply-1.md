# Custom computing task: matrix_multiply

# Sample code

[tutorial-08-matrix_multiply.cc](../../tutorial/tutorial-08-matrix_multiply.cc)

# About matrix_multiply

The program executes the multiplication of two matrices in the code and prints the result of the multiplication on the screen.

The main purpose of the example is to show how to implement a custom CPU computing task.

# Define computing task

To define a computing task, three basic information is required, i.e.: INPUT, OUTPUT, and routine.

INPUT and OUTPUT are two template parameters, which can be of any type. Routine represents the process from INPUT to OUTPUT and is defined as follows:

~~~cpp
template <class INPUT, class OUTPUT>
class __WFThreadTask
{
    ...
    std::function<void (INPUT *, OUTPUT *)> routine;
    ...
};
~~~

As shown above, routine is a simple calculation process from INPUT to OUTPUT. The INPUT pointer is not required to be const, but users can also pass const INPUT * functions.

Take an addition task for instance, you can perform in this way:

~~~cpp
struct add_input
{
    int x;
    int y;
};

struct add_ouput
{
    int res;
};

void add_routine(const add_input *input, add_output *output)
{
    output->res = input->x + input->y;
}

typedef WFThreadTask<add_input, add_output> add_task;
~~~

In matrix multiplication example, input is two matrices and output is one matrix. Its definition is as follows:

~~~cpp
namespace algorithm
{

using Matrix = std::vector<std::vector<double>>;

struct MMInput
{
    Matrix a;
    Matrix b;
};

struct MMOutput
{
    int error;
    size_t m, n, k;
    Matrix c;
};

void matrix_multiply(const MMInput *in, MMOutput *out)
{
    ...
}

}
~~~

Matrix multiplication has the problem of illegal input matrix, so an error field is added in output to indicate errors.

# Generate computing task

After defining the type of input and output and the process of algorithm, you can generate computing tasks through WFThreadTaskFactory factory.

In [WFTaskFactory.h](../src/factory/WFTaskFactory.h), computing factory class is defined as follows:

~~~cpp
template <class INPUT, class OUTPUT>
class WFThreadTaskFactory
{
private:
    using T = WFThreadTask<INPUT, OUTPUT>;

public:
    static T *create_thread_task(const std::string& queue_name,
                                 std::function<void (INPUT *, OUTPUT *)> routine,
                                 std::function<void (T *)> callback);
    ...
};
~~~

This class is a little different from the previous network factory class or algorithm factory class. It requires two template parameters, i.e.: INPUT and OUTPUT.

The knowledge about queue_name has been provided in the previous example. Routine is the computing process, callback is callback.

In the example, we can see the use of this call:

~~~cpp
using MMTask = WFThreadTask<algorithm::MMInput,
                            algorithm::MMOutput>;

using namespace algorithm;

int main()
{
    typedef WFThreadTaskFactory<MMInput, MMOutput> MMFactory;
    MMTask *task = MMFactory::create_thread_task("matrix_multiply_task",
                                                 matrix_multiply,
                                                 callback);

    MMInput *input = task->get_input();

    input->a = {{1, 2, 3}, {4, 5, 6}};
    input->b = {{7, 8}, {9, 10}, {11, 12}};
    ...
}
~~~

After the task is generated, the pointer of input data is obtained through get_input() interface. This is comparable to get_req() of network tasks.

Compared to network tasks, there is no difference in the start and end of the task. Similarly, the callback is also very simple:

~~~cpp
void callback(MMTask *task)     // MMtask = WFThreadTask<MMInput, MMOutput>
{
    MMInput *input = task->get_input();
    MMOutput *output = task->get_output();

    assert(task->get_state() == WFT_STATE_SUCCESS);

    if (output->error)
        printf("Error: %d %s\n", output->error, strerror(output->error));
    else
    {
        printf("Matrix A\n");
        print_matrix(input->a, output->m, output->k);
        printf("Matrix B\n");
        print_matrix(input->b, output->k, output->n);
        printf("Matrix A * Matrix B =>\n");
        print_matrix(output->c, output->m, output->n);
    }
}
~~~

Ordinary computing tasks can ignore the possibility of failure, and the end state must be SUCCESS.

The input and output are simply printed in the callback. If the input data is illegal, it will be printed incorrectly.

# Symmetry of algorithm and protocol

In our system, algorithms and protocols are highly symmetrical from a very abstract level.

As there are thread tasks with custom algorithms, there are also network tasks with custom protocols.

The custom algorithm requires providing the process of algorithm, while the custom protocol requires the user to provide the process of serialization and deserialization. Refer to [Simple user-defined protocol client/server](./tutorial-10-user_defined_protocol.md).

Whether it is a custom algorithm or a custom protocol, we must emphasize that both algorithm and protocol are very pure.

For example, an algorithm is a conversion process from INPUT to OUPUT, and the algorithm does not know the existence of tasks, series, etc.

In the implementation of HTTP protocol, it only cares about serialization and deserialization, and there is no need to care about what a task is. Instead, we just reference HTTP protocol in http task.

# Complexity of thread tasks and network tasks

In this example, we construct a thread task through WFThreadTaskFactory. Arguably, this is the simplest kind of computing task construction, and it is sufficient in most cases.

Similarly, users can simply define a server and client with their own protocol.

But in the previous example, we can generate a parallel sorting task through algorithm factory, and obviously we cannot do that through a routine.

For network tasks, such as a Kafka task, the results may not be obtained unless through interaction with multiple machines, but it is completely transparent to the user.

Therefore, all our tasks are complex. If you use our framework proficiently, you can design many complex components.
