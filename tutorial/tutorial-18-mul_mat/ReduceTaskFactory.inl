#define MUL_MAT_DEPTH_MAX	5
#define MUL_MAT_ROW_MIN		1
#define MUL_MAT_COLUMN_MIN	2

// from c_row_begin to c_row_end, not include c_row_end.

template<typename T>
void mulmat(T *a, T *b, T *c, size_t column_of_a, size_t column_of_b,
			size_t c_row_begin, size_t c_row_end,
			size_t c_col_begin, size_t c_col_end)
{
	fprintf(stderr, "op compute: [%zu,%zu), [%zu,%zu)\n",
			c_row_begin, c_row_end, c_col_begin, c_col_end);

	size_t k = column_of_a;
	size_t n = column_of_b;

	for (size_t i = c_row_begin; i < c_row_end; i++)
	{
		for (size_t j = c_col_begin; j < c_col_end; j++)
		{
			c[i * n + j] = 0;
			for (size_t l = 0; l < k; l++)
				c[i * n + j] += a[i * k + l] * b[l * n + j];
		}
	}
}

/********** Classes **********/

template<typename T>
class __MulMatTask : public MulMatTask<T>
{
protected:
	virtual void execute()
	{
		if (this->row_end == 0)
			this->row_end = this->output->get_row();
		if (this->col_end == 0)
			this->col_end = this->output->get_column();

		mulmat(this->input.a->get_data(),
			   this->input.b->get_data(),
			   this->output->get_data(),
			   this->input.a->get_column(),
			   this->input.b->get_column(),
			   this->row_begin,
			   this->row_end,
			   this->col_begin,
			   this->col_end);
	}

public:
	__MulMatTask(ExecQueue *queue, Executor *executor,
				 algorithm::Matrix<T> *a, algorithm::Matrix<T> *b,
				 algorithm::Matrix<T> *c,
				 mulmat_callback_t<T>&& cb) :
		MulMatTask<T>(queue, executor, std::move(cb))
	{
		this->input.a = a;
		this->input.b = b;
		this->output = c;

		this->row_begin = 0;
		this->row_end = 0;
		this->col_begin = 0;
		this->col_end = 0;
	}

	__MulMatTask(ExecQueue *queue, Executor *executor,
				 algorithm::Matrix<T> *a, algorithm::Matrix<T> *b,
				 algorithm::Matrix<T> *c,
				 size_t row_begin, size_t row_end,
				 size_t col_begin, size_t col_end,
				 mulmat_callback_t<T>&& cb) :
		MulMatTask<T>(queue, executor, std::move(cb))
	{
		this->input.a = a;
		this->input.b = b;
		this->output = c;

		this->row_begin = row_begin;
		this->row_end = row_end;
		this->col_begin = col_begin;
		this->col_end = col_end;
	}

protected:
	size_t row_begin;
	size_t row_end;
	size_t col_begin;
	size_t col_end;
};

template<typename T>
class __ParMulMatTask : public __MulMatTask<T>
{
public:
	virtual void dispatch();

protected:
	virtual SubTask *done()
	{
		if (this->flag)
			return series_of(this)->pop();

		assert(this->state == WFT_STATE_SUCCESS);
		return this->MulMatTask<T>::done();
	}

	virtual void execute();

protected:
	int depth;
	int flag;

public:
	__ParMulMatTask(ExecQueue *queue, Executor *executor,
				 	algorithm::Matrix<T> *a, algorithm::Matrix<T> *b,
					algorithm::Matrix<T> *c,
					int depth, mulmat_callback_t<T>&& cb) :
		__MulMatTask<T>(queue, executor,
						a, b, c,
						std::move(cb))
	{
		this->depth = depth;
		this->flag = 0;
	}

	__ParMulMatTask(ExecQueue *queue, Executor *executor,
				 	algorithm::Matrix<T> *a, algorithm::Matrix<T> *b,
					algorithm::Matrix<T> *c,
					size_t row_begin, size_t row_end,
					size_t col_begin, size_t col_end,
					int depth, mulmat_callback_t<T>&& cb) :
		__MulMatTask<T>(queue, executor,
						a, b, c,
						row_begin, row_end,
						col_begin, col_end,
						std::move(cb))
	{
		this->depth = depth;
		this->flag = 0;
	}

private:
	bool split(size_t row, size_t column)
	{
		return row > 1 && column > 1 &&
			   (row >= MUL_MAT_ROW_MIN && column >= MUL_MAT_COLUMN_MIN);
	}
};

template<typename T>
void __ParMulMatTask<T>::dispatch()
{
	if (this->row_end == 0)
		this->row_end = this->output->get_row();
	if (this->col_end == 0)
		this->col_end = this->output->get_column();

	size_t row = this->row_end - this->row_begin;
	size_t col = this->col_end - this->col_begin;

	if (!this->flag && this->depth < MUL_MAT_DEPTH_MAX && this->split(row, col))
	{
		SeriesWork *series = series_of(this);
		__ParMulMatTask<T> *task1;
		__ParMulMatTask<T> *task2;
		size_t mid;

		if (row >= col)
		{
			mid = this->row_begin + row / 2;
			task1 = new __ParMulMatTask<T>(this->queue, this->executor,
										   this->input.a, this->input.b,
										   this->output,
										   this->row_begin, mid,
										   this->col_begin, this->col_end,
										   this->depth + 1, nullptr);

			task2 = new __ParMulMatTask<T>(this->queue, this->executor,
										   this->input.a, this->input.b,
										   this->output,
										   mid, this->row_end,
										   this->col_begin, this->col_end,
										   this->depth + 1, nullptr);
		}
		else
		{
			mid = this->col_begin + col / 2;
			task1 = new __ParMulMatTask<T>(this->queue, this->executor,
										   this->input.a, this->input.b,
										   this->output,
										   this->row_begin, this->row_end,
										   this->col_begin, mid,
										   this->depth + 1, nullptr);

			task2 = new __ParMulMatTask<T>(this->queue, this->executor,
										   this->input.a, this->input.b,
										   this->output,
										   this->row_begin, this->row_end,
										   mid, this->col_end,
										   this->depth + 1, nullptr);
		}
		SeriesWork *sub_series[2] = {
			Workflow::create_series_work(task1, nullptr),
			Workflow::create_series_work(task2, nullptr)
		};
		ParallelWork *parallel =
			Workflow::create_parallel_work(sub_series, 2, nullptr);

		series->push_front(this);
		series->push_front(parallel);
		this->flag = 1;
		this->subtask_done();
	}
	else
		this->__MulMatTask<T>::dispatch();
}

template<typename T>
void __ParMulMatTask<T>::execute()
{
	if (this->flag)
		this->flag = 0; // merge : do nothing
	else
		this->__MulMatTask<T>::execute();
}

/********** Factory functions **********/

template<typename T, class CB>
MulMatTask<T> *ReduceTaskFactory::create_mulmat_task(const std::string& name,
													 algorithm::Matrix<T> *a,
													 algorithm::Matrix<T> *b,
													 algorithm::Matrix<T> *c,
													 CB callback)
{
	return new __MulMatTask<T>(WFGlobal::get_exec_queue(name),
							   WFGlobal::get_compute_executor(),
							   a, b, c,
							   std::move(callback));
}

template<typename T, class CB>
MulMatTask<T> *ReduceTaskFactory::create_pmulmat_task(const std::string& name,
													  algorithm::Matrix<T> *a,
													  algorithm::Matrix<T> *b,
													  algorithm::Matrix<T> *c,
													  CB callback)
{
	return new __ParMulMatTask<T>(WFGlobal::get_exec_queue(name),
								  WFGlobal::get_compute_executor(),
								  a, b, c, 0,
								  std::move(callback));
}

