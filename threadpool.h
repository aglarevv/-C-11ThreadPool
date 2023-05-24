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

const int TASK_MAX_THRESHOLD = 2;//INT32_MAX;	//������������
const int THREAD_MAX_THRESHOLD = 100;		//�߳���������
const int IDLETHREAD_LIMITS = 10;			//�����̵߳ȴ�ʱ������

enum class PoolMode {
	MODE_FIXED,	//�̶��߳�����
	MODE_CACHED	//�߳�������̬����
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
		//�����߳�
		for (int i = 0; i < initThreadSize; ++i) {
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			auto threadId = ptr->getId();
			thread_.emplace(threadId, std::move(ptr));
			//thread_.emplace_back(std::move(ptr));	
			idleThread_++;
			//thread_.emplace_back(new Thread(std::bind(&threadpool::threadFunc, this)));
		}
		//�����߳�
		for (int i = 0; i < initThreadSize; ++i) {
			thread_[i]->start();
		}
	}

	template<typename Func , typename... Args>
	auto submitTask(Func &&func, Args&&...args) -> std::future<decltype(func(args...))> {
		//�������
		using Rtype = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<Rtype()>>(
			std::bind(std::forward<Func>(func),std::forward<Args>(args)...));
		std::future<Rtype> result = task->get_future();

		std::unique_lock<std::mutex> lock(taskQueMtx_);//��ȡ��
		//����ȴ�ʱ�䳬��1��,�����ύʧ��
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQueue_.size() < (size_t)taskQueMaxThreshold_; })) {
			std::cerr << "submit task error\n";
			auto task = std::make_shared<std::packaged_task<Rtype()>>([]()->Rtype {return Rtype(); });
			(*task)();
			return task->get_future();
		}
		//�����������
		taskQueue_.emplace([task]() {(*task)(); });
		taskSize_++;
		//֪ͨ�̳߳�������в�����,��������
		notEmpty_.notify_all();

		//catchedģʽ
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
		//��ʱ��
		std::chrono::time_point lastTime = std::chrono::high_resolution_clock().now();
		//����ִ������������,�̳߳ز���������
		while (true)
		{
			Task task;
			{
				//�Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);
				std::cout << "���Ի�ȡ����tid " << std::this_thread::get_id() << std::endl;

				//������ʱ
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
							if (dur.count() > IDLETHREAD_LIMITS //catchģʽѭ���ȴ�ʱ�����ܳ���60s
								&& currentThreadSize_ > initThreadSize_) {
								//û������ʱ���տ����߳�
								thread_.erase(threadId);
								currentThreadSize_--;
								idleThread_--;
								std::cout << "delete thread id : " << std::this_thread::get_id() << std::endl;
								return;
							}
						}
					}
					//fixedģʽѭ���ȴ�
					else {
						notEmpty_.wait(lock);
					}
				}

				std::cout << "��ȡ����tid " << std::this_thread::get_id() << std::endl;

				//��ȡ����
				task = taskQueue_.front();
				taskQueue_.pop();
				taskSize_--;
				idleThread_--;

				//�����������,��֪ͨ�����ȴ����߳�
				if (taskQueue_.size() > 0) {
					notEmpty_.notify_all();
				}
				//֪ͨ���Լ����ύ����
				notFull_.notify_all();

			}
			//ִ��
			if (task != nullptr)
				task();
			idleThread_++;
			//���¼�ʱ��
			lastTime = std::chrono::high_resolution_clock().now();
		}
	}
	bool checkRunningState()const{ return isRunning_; }

private:
	std::unordered_map<unsigned int, std::unique_ptr<Thread>> thread_;
	int initThreadSize_;	//��ʼ�߳�����
	std::atomic_int currentThreadSize_; //��ǰ�߳�������
	std::atomic_int idleThread_; //�����߳�����
	int threadMaxThreshold_; //�߳�����������ֵ

	using Task = std::function<void()>;//ʹ���м��
	std::queue<Task> taskQueue_;	//�������
	std::atomic_int taskSize_;	//��������
	int taskQueMaxThreshold_;	//�������������ֵ

	std::mutex taskQueMtx_;
	std::condition_variable notFull_;	//������в���
	std::condition_variable notEmpty_;	//������в���

	std::condition_variable exitCond_;
	PoolMode poolmode_;	//�̳߳�ģʽ
	std::atomic_bool isRunning_; //�̳߳�����״̬

};




#endif 