#include "threadpool.h"

#include <iostream>
#include <functional>
#include <thread>


const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 200;
const int THREAD_MAX_IDLE_TIME = 60; // Unit: second

//********************************* Result *************************************//

Result::Result(std::shared_ptr<Task> task, bool isValid)
	: isValid_(isValid)
	, task_(task)
{
	task_->setResult(this);
}

Any Result::get()
{
	if (!isValid_)
	{
		return "";
	}
	sem_.wait();
	return std::move(any_);
}

void Result::setVal(Any any)
{
	this->any_ = std::move(any);
	sem_.post();
}

//********************************* Task *****************************//
void Task::exec()
{
	if (result_ != nullptr)
		result_->setVal(run());
}

void Task::setResult(Result* res)
{
	result_ = res;
}

Task::Task() : result_(nullptr)
{}

//********************************* Thread 类 **************************//
int Thread::generateID_ = 0;

Thread::Thread(ThreadFunc func)
	: func_(func)
	, threadID_(generateID_++)
{}

Thread::~Thread()
{}

void Thread::start()
{
	// 创建一个线程来执行线程函数
	std::thread t(func_, threadID_); // 线程对象t 和线程函数func_
	t.detach(); // 分离线程, 防止作用域结束，线程挂掉
}

int Thread::getID()const
{
	return threadID_;
}

//****************************** ThreadPool ****************************//
ThreadPool::ThreadPool()
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

ThreadPool::~ThreadPool()
{
	isPoolRunning_ = false;
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	exitCond_.wait(lock, [&]() {return threads_.size() == 0; });
}

void ThreadPool::setMode(PoolMode mode)
{
	if (checkRunningState())
		return;
	poolMode_ = mode;
}

void ThreadPool::setThreadMaxThreshHold(int threshHold)
{
	if (checkRunningState())
		return;
	if (poolMode_ == PoolMode::MODE_CACHED)
		threadMaxThreshHold_ = threshHold;
}

void ThreadPool::setTaskQueMaxThreshHold(int threshHold)
{
	if (checkRunningState())
		return;
	initThreadSize_ = threshHold;
}

Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	//获取锁
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	//线程通信 等待任务放入任务队列（队列阈值） 超时返回任务提交失败

	if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() {
		return taskQue_.size() < taskQueMaxThreshHold_;}))
	{
		std::cerr << "task queue is full, submit failed." << std::endl;
		return Result(sp, false);
	}
	
	//队列空余，放任务
	taskQue_.emplace(sp);
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

	return Result(sp);

}

void ThreadPool::start(int initThreadSize)
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

bool ThreadPool::checkRunningState() const
{
	return isPoolRunning_;
}

void ThreadPool::threadFunc(int threadID)
{
	auto lastTime = std::chrono::high_resolution_clock().now();

	for(;;)
	{
		std::shared_ptr<Task> task;
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

				if (poolMode_ == PoolMode::MODE_CACHED){
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
			task->exec(); // 增加的功能，套一个壳子
		}

		idleThreadSize_++; // 任务结束，空间线程++
		lastTime = std::chrono::high_resolution_clock().now(); // 更新执行完任务的时间
		
	}
}
