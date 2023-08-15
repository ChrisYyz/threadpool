#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory> // 智能指针
#include <atomic> // 原子操作 - 轻量锁
#include <mutex> // 互斥锁
#include <condition_variable> // 条件变量
#include <functional>
#include <unordered_map>

/*
Ex:
ThreadPool pool;
pool.start(6);

class MyTask : public task
{
public:
	void run()
	{
		.....
	}
};

pool.submitTask(std::make_shared<MyTask>());

*/

// 信号量 semaphore
class Semaphore
{
public:
	Semaphore(int limit = 0) : resLimit_(limit)
	{}
	~Semaphore() = default;
	// 获取信号量
	void wait()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
		resLimit_--;
	}

	void post()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		cond_.notify_all();
	}

private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

// 返回结果，Any 类，类似java中的object 模板类实现只能写在头文件中
class Any
{
public:
	Any() = default;
	~Any() = default;

	//unique_ptr 禁止左值拷贝和赋值重载
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;

	// rvalue copy construct
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	template<class T>
	Any(T data) : base_(std::make_unique<Derive<T>>(data))
	{ }

	// grab the data
	template<class T>
	T cast_()
	{
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr)
		{
			throw "type not matched";
		}
		return pd->data_;
	}
private:
	// Base class
	class Base
	{
	public:
		virtual ~Base() = default;
	};

	template<class T>
	class Derive : public Base
	{
	public:
		Derive(T data) : data_(data)
		{}
		T data_;
	};
private:
	std::unique_ptr<Base> base_;
};


// result类，接收返回结果
class Task;

class Result
{
public:
	Result(std::shared_ptr<Task> task, bool isValid = true);
	~Result() = default;

	void setVal(Any any);

	Any get();
private:
	Any any_;
	Semaphore sem_;
	std::shared_ptr<Task> task_;
	std::atomic_bool isValid_;
};


// 抽象类，多态task.run()
class Task
{
public:
	Task();
	~Task() = default;
	void exec();
	void setResult(Result* res);

	virtual Any run() = 0; //纯虚函数， 自定义任务执行方式
private:
	Result* result_;
};

//线程类
class Thread
{
public:
	using ThreadFunc = std::function<void(int)>; // 线程函数对象类型
	void start();

	Thread(ThreadFunc);
	~Thread();

	int getID()const;

private:
	ThreadFunc func_;
	static int generateID_;
	int threadID_; // 保存线程ID；
};

// 线程池

enum class PoolMode
{
	MODE_FIXED,  // 固定数量
	MODE_CACHED, // 动态增长
};

class ThreadPool
{
public:
	ThreadPool();
	~ThreadPool();

	void setMode(PoolMode mode);

	void setTaskQueMaxThreshHold(int threshHold); // 自定义最大阈值
	void setThreadMaxThreshHold(int threshHold); // 自定义线程最大阈值

	Result submitTask(std::shared_ptr<Task> sp);

	void start(int initThreadSize = std::thread::hardware_concurrency());

	// 禁止使用
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 定义线程函数
	void threadFunc(int threadID);

	//检查pool运行状态
	bool checkRunningState() const;

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表
	size_t initThreadSize_; // initialize thread number, unsigned int
	
	std::atomic_int curThreadSize_; // 当前线程数量
	int threadMaxThreshHold_; // 线程上线阈值	
	std::atomic_int idleThreadSize_; // 空闲线程数量

	std::queue<std::shared_ptr<Task>> taskQue_; // task queue, auto ptr, lengthing the life cycle of tasks
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

