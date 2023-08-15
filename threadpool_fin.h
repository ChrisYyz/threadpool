#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory> 
#include <atomic> 
#include <mutex> 
#include <condition_variable> 
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 200;
const int THREAD_MAX_IDLE_TIME = 60; // Unit: second

//线程类
class Thread
{
public:
	using ThreadFunc = std::function<void(int)>; // 线程函数对象类型
	void start()
	{
		// 创建一个线程来执行线程函数
		std::thread t(func_, threadID_); // 线程对象t 和线程函数func_
		t.detach(); // 分离线程, 防止作用域结束，线程挂掉
	}

	Thread(ThreadFunc func)
		: func_(func)
		, threadID_(generateID_++)
	{}
	~Thread() = default;

	int getID()const
	{
		return threadID_;
	}

private:
	ThreadFunc func_;
	static int generateID_;
	int threadID_; // 保存线程ID；
};

int Thread::generateID_ = 0;

// ThreadPool 

enum class PoolMode
{
	MODE_FIXED,  // 固定数量
	MODE_CACHED, // 动态增长
};

class ThreadPool
{
public:
	ThreadPool()
		: initThreadSize_(0)
		, taskSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
		, idleThreadSize_(0)
		, threadMaxThreshHold_(THREAD_MAX_THRESHHOLD)
		, curThreadSize_(0)
	{
	}

	~ThreadPool()
	{
		isPoolRunning_ = false;
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]() {return threads_.size() == 0; });
	}

	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	void setTaskQueMaxThreshHold(int threshHold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshHold;
	}

	void setThreadMaxThreshHold(int threshHold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
			threadMaxThreshHold_ = threshHold;
	}

	//使用可变参模板编程，接受任意函数和不同数量的参数
	template<class Func, class... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// package task -> into task queue
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)); // 绑定器， 参数绑定在函数对象上
		std::future<RType> result = task->get_future();

		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//线程通信 等待任务放入任务队列（队列阈值） 超时返回任务提交失败

		if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() {
			return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			std::cerr << "task queue is full, submit failed." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		//队列空余，放任务
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		taskQue_.emplace([task]() {(*task)();}); // labmda
		taskSize_++;

		// 状态notFull notEmpty - 通知线程
		notEmpty_.notify_all();
		// cached模式 - 是否需要创建新线程
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadMaxThreshHold_)
		{
			//创建对象
			std::cout << "create new thread" << std::endl;
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadID = ptr->getID();
			threads_.emplace(threadID, std::move(ptr));
			//启动
			threads_[threadID]->start();
			// Update Thread status
			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
	}

	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//设置线程池状态
		isPoolRunning_ = true;
		// 线程初始个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//创建线程对象
		for (int i = 0; i < initThreadSize_; i++)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadID = ptr->getID();
			threads_.emplace(threadID, std::move(ptr));
		}

		// 启动线程
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();
			idleThreadSize_++;
		}
	}

	// 禁止使用
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 定义线程函数
	void threadFunc(int threadID)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		for (;;)
		{
			Task task;
			{
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "尝试获取任务" << std::this_thread::get_id() << std::endl;
				// cached模式
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadID);
						exitCond_.notify_all();
						std::cout << "thread deleted :" << std::this_thread::get_id() << std::endl;
						return;
					}

					if (poolMode_ == PoolMode::MODE_CACHED) {
						// 条件变量超时返回、删除多余线程
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto nowTime = std::chrono::high_resolution_clock().now();
							auto duration = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
							if (duration.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								//回收当前线程
								/* 数量维护， 线程容器中删除线程, 线程ID*/
								threads_.erase(threadID);
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "thread deleted :" << std::this_thread::get_id() << std::endl;
								return;
							}
						}
					}
					else // fixed 模式
					{
						notEmpty_.wait(lock);
					}
				}
				// get task
				idleThreadSize_--; // 空闲线程数量--
				std::cout << "获取任务成功" << std::this_thread::get_id() << std::endl;
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//如果依然有剩余任务，通知其他线程执行任务
				if (!taskQue_.size() > 0)
					notEmpty_.notify_all();

				// 可以继续提交任务进入队列
				notFull_.notify_all();
			}
			// 操作完任务队列归还锁;
			// 
			// 执行任务 exec task
			if (task != nullptr)
			{
				task(); // 执行匿名函数对象
			}

			idleThreadSize_++; // 任务结束，空间线程++
			lastTime = std::chrono::high_resolution_clock().now(); // 更新执行完任务的时间

		}
	}

	//检查pool运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表
	size_t initThreadSize_; // initialize thread number, unsigned int

	std::atomic_int curThreadSize_; // 当前线程数量
	int threadMaxThreshHold_; // 线程上线阈值	
	std::atomic_int idleThreadSize_; // 空闲线程数量

	// Task -> func
	using Task = std::function<void()>; // middle class, return value unknown
	std::queue<Task> taskQue_; // task queue, auto ptr, lengthing the life cycle of tasks
	std::atomic_int taskSize_; // task numbers
	int taskQueMaxThreshHold_; // 任务队列上限阈值

	PoolMode poolMode_;

	std::mutex taskQueMtx_; //任务队列 互斥锁
	std::condition_variable notFull_; // 任务队列没满
	std::condition_variable notEmpty_; // 任务队列没空
	std::condition_variable exitCond_; // 结束等待线程返回

	std::atomic_bool isPoolRunning_; // 工作状态

};

#endif

