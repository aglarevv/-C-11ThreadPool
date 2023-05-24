#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <future>
#include <thread>
#include "threadpool.h"
#include <thread>


int sum(int a, int b) {
	std::this_thread::sleep_for(std::chrono::seconds(5));
	return a + b;
}

int main(void)
{
	{
		ThreadPool pool;
		//pool.setMode(PoolMode::MODE_CACHED);
		pool.start(2);
		std::future<int> ret = pool.submitTask(sum, 10, 20);
		std::future<int> ret0 = pool.submitTask(sum, 10, 20);
		std::future<int> ret9 = pool.submitTask(sum, 10, 20);
		std::future<int> ret2 = pool.submitTask(sum, 10, 20);
		std::future<int> ret3 = pool.submitTask(sum, 10, 20);
		std::future<int> ret4 = pool.submitTask([]()->int {std::this_thread::
			sleep_for(std::chrono::seconds(2)); return 15 * 15; });
		std::cout << ret.get();
		std::cout << ret4.get() << std::endl;

	}
	
	getchar();

	return EXIT_SUCCESS;
}

