#include <random>
#ifndef _REDUCETASKFACTORY_H_

namespace algorithm
{

// TODO: use function pointer to init data
enum InitType
{
	Zero = 0,
	Rand = 1,
	None = 2,
};

template<typename T>
class Matrix
{
public:
	Matrix(enum InitType type, size_t row, size_t column)
	{
		this->data = (T *)malloc(row * column * sizeof (T));

		if (this->data)
		{
			this->row = row;
			this->col = column;
			std::default_random_engine random_engine;
			std::uniform_real_distribution<T> urandom_gen(0.0, 1000.0);

			switch(type)
			{
			case Zero:
				memset(this->data, 0, row * column * sizeof(T));
				break;
			case Rand:
				for (size_t i = 0; i < row * col; i++)
					this->data[i] = urandom_gen(random_engine);
				break;
			case None:
			default:
				break;
			}
		}
		else
		{
			this->data = NULL;
			this->row = 0;
			this->col = 0;
		}
	}

	~Matrix()
	{
		if (this->data)
			free(this->data);
	}

	T *get_data() { return this->data; }
	size_t get_row() { return this->row; }
	size_t get_column() { return this->col; }

private:
	T *data;
	size_t row, col;
};

template<typename T>
struct MatrixIn
{
	Matrix<T> *a;
	Matrix<T> *b;
};

// typedef Matrix MatrixOut;
template<typename T>
using MatrixOut = Matrix<T> *;

} /* namespace algorithm */

template<typename T>
using MulMatTask = WFThreadTask<algorithm::MatrixIn<T>,
								algorithm::MatrixOut<T>>;

template<typename T>
using mulmat_callback_t = std::function<void (MulMatTask<T> *)>;

class ReduceTaskFactory
{
public:
	template<typename T, class CB = mulmat_callback_t<T>>
	static MulMatTask<T> *create_mulmat_task(const std::string& queue_name,
											 algorithm::Matrix<T> *a,
											 algorithm::Matrix<T> *b,
											 algorithm::Matrix<T> *c,
											 CB callback);

	template<typename T, class CB = mulmat_callback_t<T>>
	static MulMatTask<T> *create_pmulmat_task(const std::string& queue_name,
											  algorithm::Matrix<T> *a,
											  algorithm::Matrix<T> *b,
											  algorithm::Matrix<T> *c,
											  CB callback);
};

#include "ReduceTaskFactory.inl"
#endif

