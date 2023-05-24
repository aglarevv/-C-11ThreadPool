#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <mutex>
#include <memory>
#include <queue>
#include <atomic>
#include <vector>
#include <condition_variable>
#include <functional>
#include <future>
#include <unordered_map>

const int TASK_MAX_THRESHOLD = 2;//INT32_MAX;	//任务数量上限
const int THREAD_MAX_THRESHOLD = 100;		//线程数量上限
const int IDLETHREAD_LIMITS = 10;			//空闲线程等待时间上限

enum class PoolMode {
	MODE_FIXED,	//固定线程数量
	MODE_CACHED	//线程数量动态增长
};

class Thread {
public:
	using ThreadFunc = std::function<void(unsigned int)>;
	Thread(ThreadFunc func) :
		func_(func),
		threadId_(generateId_++)
	{}
	~Thread() = default;
	void start(){
		std::thread t(func_, threadId_);
		t.detach();
	}
	unsigned int getId()const{ return threadId_; }
private:
	ThreadFunc func_;
	static unsigned int generateId_;
	unsigned int threadId_;
};
unsigned int Thread::generateId_ = 0;

class ThreadPool {
public:
	ThreadPool():
		initThreadSize_(0),
			currentThreadSize_(0),
			idleThread_(0),
			threadMaxThreshold_(THREAD_MAX_THRESHOLD),
			taskQueMaxThreshold_(TASK_MAX_THRESHOLD),
			taskSize_(0),
			poolmode_(PoolMode::MODE_FIXED),
			isRunning_(false)
	{}
	~ThreadPool(){
		isRunning_ = false;

		notEmpty_.notify_all();
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		exitCond_.wait(lock, [&]()->bool {return thread_.size() == 0; });
	}
	void start(int initThreadSize = std::thread::hardware_concurrency()){
		isRunning_ = true;
		initThreadSize_ = initThreadSize;
		currentThreadSize_ = initThreadSize;
		//创建线程
		for (int i = 0; i < initThreadSize; ++i) {
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			auto threadId = ptr->getId();
			thread_.emplace(threadId, std::move(ptr));
			//thread_.emplace_back(std::move(ptr));	
			idleThread_++;
			//thread_.emplace_back(new Thread(std::bind(&threadpool::threadFunc, this)));
		}
		//启动线程
		for (int i = 0; i < initThreadSize; ++i) {
			thread_[i]->start();
		}
	}

	template<typename Func , typename... Args>
	auto submitTask(Func &&func, Args&&...args) -> std::future<decltype(func(args...))> {
		//打包任务
		using Rtype = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<Rtype()>>(
			std::bind(std::forward<Func>(func),std::forward<Args>(args)...));
		std::future<Rtype> result = task->get_future();

		std::unique_lock<std::mutex> lock(taskQueMtx_);//获取锁
		//任务等待时间超过1秒,任务提交失败
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQueue_.size() < (size_t)taskQueMaxThreshold_; })) {
			std::cerr << "submit task error\n";
			auto task = std::make_shared<std::packaged_task<Rtype()>>([]()->Rtype {return Rtype(); });
			(*task)();
			return task->get_future();
		}
		//加入任务队列
		taskQueue_.emplace([task]() {(*task)(); });
		taskSize_++;
		//通知线程池任务队列不空了,有任务了
		notEmpty_.notify_all();

		//catched模式
		if (poolmode_ == PoolMode::MODE_CACHED && taskSize_ > idleThread_
			&& currentThreadSize_ < threadMaxThreshold_) {
			std::cout << "creat new thread\n";
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, 
				this, std::placeholders::_1));
			auto threadId = ptr->getId();
			thread_.emplace(threadId, std::move(ptr));
			thread_[threadId]->start();
			currentThreadSize_++;
			idleThread_++;
		}

		return result;
	}
	void setMode(PoolMode poolmode){
		if (checkRunningState())
			return;
		poolmode_ = poolmode;
	}
	void setTaskQueMaxThreshold_(int threshold){
		if (checkRunningState())
			return;
		taskQueMaxThreshold_ = threshold;
	}
	void setInitThreshold(int size){
		if (checkRunningState())
			return;
		initThreadSize_ = size;
	}
	void setThreadMaxThreshold(int Threshold){
		if (checkRunningState())
			return;
		if (poolmode_ == PoolMode::MODE_CACHED)
			threadMaxThreshold_ = Threshold;
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool) = delete;

private:
	void threadFunc(unsigned int threadId){
		//计时器
		std::chrono::time_point lastTime = std::chrono::high_resolution_clock().now();
		//必须执行完所有任务,线程池才允许析构
		while (true)
		{
			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);
				std::cout << "尝试获取任务tid " << std::this_thread::get_id() << std::endl;

				//无任务时
				while (taskQueue_.size() == 0) {

					if (!isRunning_) {
						thread_.erase(threadId);
						std::cout << "exit thread\n" << std::this_thread::get_id() << std::endl;
						exitCond_.notify_all();
						return;
					}

					if (poolmode_ == PoolMode::MODE_CACHED) {						
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() > IDLETHREAD_LIMITS //catch模式循环等待时长不能超过60s
								&& currentThreadSize_ > initThreadSize_) {
								//没有任务时回收空闲线程
								thread_.erase(threadId);
								currentThreadSize_--;
								idleThread_--;
								std::cout << "delete thread id : " << std::this_thread::get_id() << std::endl;
								return;
							}
						}
					}
					//fixed模式循环等待
					else {
						notEmpty_.wait(lock);
					}
				}

				std::cout << "获取任务到tid " << std::this_thread::get_id() << std::endl;

				//获取任务
				task = taskQueue_.front();
				taskQueue_.pop();
				taskSize_--;
				idleThread_--;

				//如果还有任务,就通知其他等待的线程
				if (taskQueue_.size() > 0) {
					notEmpty_.notify_all();
				}
				//通知可以继续提交任务
				notFull_.notify_all();

			}
			//执行
			if (task != nullptr)
				task();
			idleThread_++;
			//更新计时器
			lastTime = std::chrono::high_resolution_clock().now();
		}
	}
	bool checkRunningState()const{ return isRunning_; }

private:
	std::unordered_map<unsigned int, std::unique_ptr<Thread>> thread_;
	int initThreadSize_;	//初始线程数量
	std::atomic_int currentThreadSize_; //当前线程总数量
	std::atomic_int idleThread_; //空闲线程数量
	int threadMaxThreshold_; //线程数量上限阈值

	using Task = std::function<void()>;//使用中间层
	std::queue<Task> taskQueue_;	//任务队列
	std::atomic_int taskSize_;	//任务数量
	int taskQueMaxThreshold_;	//任务队列上限阈值

	std::mutex taskQueMtx_;
	std::condition_variable notFull_;	//任务队列不满
	std::condition_variable notEmpty_;	//任务队列不空

	std::condition_variable exitCond_;
	PoolMode poolmode_;	//线程池模式
	std::atomic_bool isRunning_; //线程池运行状态

};




#endif 