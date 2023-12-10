#include <random>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include "workflow/WFAlgoTaskFactory.h"
#include "workflow/WFFacilities.h"

#include "ReduceTaskFactory.h"

using namespace algorithm;

static WFFacilities::WaitGroup wait_group(1);

bool use_parallel_mulmat = false;

// cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, m, n, k, 1.0, a, k, b, n, 0.0, c, n);

template<typename T>
void print_matrix(T *m, size_t row, size_t col)
{
	std::cout << "Matrix(" << row << " ,"<<col<<"):\n[" << std::endl;
	for (size_t i = 0; i < row; i++) {
		std::cout << "  [ ";
		for (size_t j = 0; j < col; j++) {
			std::cout <<  m[i * col + j] << " ";
		}
		std::cout << "]" << std::endl;
	}
	std::cout << "]" << std::endl;
}

template<typename T>
void mul_mat_cpu(const T *a, const T *b, const T *c, size_t width)
{
	for (size_t i = 0; i < width; i++)
	{
		for (size_t j = 0; j < width; j++)
		{
			T sum = 0;
			for (size_t k = 0; k < width; k++)
			{
				T tmp_a = a[i * width + k];
				T tmp_b = b[k * width + j];
				sum += tmp_a * tmp_b;
			}
		}
	}
}

void callback(MulMatTask<float> *task)
{
	Matrix<float> **output = task->get_output();
	const float *p = (*output)->get_data();
	size_t row = (*output)->get_row();
	size_t column = (*output)->get_column();

	print_matrix(p, row, column);

	printf("done\n");
	wait_group.done();
}

int main(int argc, char *argv[])
{
	size_t m, k, n;

	if (argc != 4 && argc != 5)
	{
		fprintf(stderr, "DESCRIPTION: Matrix Multiplication A(m,k) * B(k,n) = C(m,n)\n");
		fprintf(stderr, "USAGE: %s <m> <k> <n> [p]\n", argv[0]);
		exit(1);
	}

	m = atoi(argv[1]);
	k = atoi(argv[2]);
	n = atoi(argv[3]);

	if (m * k * n == 0)
	{
		fprintf(stderr, "Invalid parameters!\n");
		exit(1);
	}

	Matrix<float> a(Rand, m, k);
	Matrix<float> b(Rand, k, n);
	Matrix<float> c(Zero, m, n);

	if (argc == 5 && (*argv[4] == 'p' || *argv[4] == 'P'))
		use_parallel_mulmat = true;

	MulMatTask<float> *task;
	if (use_parallel_mulmat)
		task = ReduceTaskFactory::create_pmulmat_task("mm", &a, &b, &c, callback);
	else
		task = ReduceTaskFactory::create_mulmat_task("mm", &a, &b, &c, callback);

	if (use_parallel_mulmat)
		printf("Start matrix muliplication parallelly...\n");
	else
		printf("Start matrix muliplication...\n");

	task->start();

	wait_group.wait();
	return 0;
}

