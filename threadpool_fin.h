#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory> // ����ָ��
#include <atomic> // ԭ�Ӳ��� - ������
#include <mutex> // ������
#include <condition_variable> // ��������
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 200;
const int THREAD_MAX_IDLE_TIME = 60; // Unit: second

//�߳���
class Thread
{
public:
	using ThreadFunc = std::function<void(int)>; // �̺߳�����������
	void start()
	{
		// ����һ���߳���ִ���̺߳���
		std::thread t(func_, threadID_); // �̶߳���t ���̺߳���func_
		t.detach(); // �����߳�, ��ֹ������������̹߳ҵ�
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
	int threadID_; // �����߳�ID��
};

int Thread::generateID_ = 0;

// ThreadPool 

enum class PoolMode
{
	MODE_FIXED,  // �̶�����
	MODE_CACHED, // ��̬����
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

	//ʹ�ÿɱ��ģ���̣��������⺯���Ͳ�ͬ�����Ĳ���
	template<class Func, class... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// package task -> into task queue
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)); // ������ �������ں���������
		std::future<RType> result = task->get_future();

		//��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//�߳�ͨ�� �ȴ��������������У�������ֵ�� ��ʱ���������ύʧ��

		if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() {
			return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			std::cerr << "task queue is full, submit failed." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		//���п��࣬������
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		taskQue_.emplace([task]() {(*task)();}); // labmda
		taskSize_++;

		// ״̬notFull notEmpty - ֪ͨ�߳�
		notEmpty_.notify_all();
		// cachedģʽ - �Ƿ���Ҫ�������߳�
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadMaxThreshHold_)
		{
			//��������
			std::cout << "create new thread" << std::endl;
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadID = ptr->getID();
			threads_.emplace(threadID, std::move(ptr));
			//����
			threads_[threadID]->start();
			// Update Thread status
			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
	}

	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//�����̳߳�״̬
		isPoolRunning_ = true;
		// �̳߳�ʼ����
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//�����̶߳���
		for (int i = 0; i < initThreadSize_; i++)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadID = ptr->getID();
			threads_.emplace(threadID, std::move(ptr));
		}

		// �����߳�
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();
			idleThreadSize_++;
		}
	}

	// ��ֹʹ��
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// �����̺߳���
	void threadFunc(int threadID)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		for (;;)
		{
			Task task;
			{
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "���Ի�ȡ����" << std::this_thread::get_id() << std::endl;
				// cachedģʽ
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
						// ����������ʱ���ء�ɾ�������߳�
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto nowTime = std::chrono::high_resolution_clock().now();
							auto duration = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
							if (duration.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								//���յ�ǰ�߳�
								/* ����ά���� �߳�������ɾ���߳�, �߳�ID*/
								threads_.erase(threadID);
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "thread deleted :" << std::this_thread::get_id() << std::endl;
								return;
							}
						}
					}
					else // fixed ģʽ
					{
						notEmpty_.wait(lock);
					}
				}
				// get task
				idleThreadSize_--; // �����߳�����--
				std::cout << "��ȡ����ɹ�" << std::this_thread::get_id() << std::endl;
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//�����Ȼ��ʣ������֪ͨ�����߳�ִ������
				if (!taskQue_.size() > 0)
					notEmpty_.notify_all();

				// ���Լ����ύ����������
				notFull_.notify_all();
			}
			// ������������й黹��;
			// 
			// ִ������ exec task
			if (task != nullptr)
			{
				task(); // ִ��������������
			}

			idleThreadSize_++; // ����������ռ��߳�++
			lastTime = std::chrono::high_resolution_clock().now(); // ����ִ���������ʱ��

		}
	}

	//���pool����״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // �߳��б�
	size_t initThreadSize_; // initialize thread number, unsigned int

	std::atomic_int curThreadSize_; // ��ǰ�߳�����
	int threadMaxThreshHold_; // �߳�������ֵ	
	std::atomic_int idleThreadSize_; // �����߳�����

	// Task -> func
	using Task = std::function<void()>; // middle class, return value unknown
	std::queue<Task> taskQue_; // task queue, auto ptr, lengthing the life cycle of tasks
	std::atomic_int taskSize_; // task numbers
	int taskQueMaxThreshHold_; // �������������ֵ

	PoolMode poolMode_;

	std::mutex taskQueMtx_; //������� ������
	std::condition_variable notFull_; // �������û��
	std::condition_variable notEmpty_; // �������û��
	std::condition_variable exitCond_; // �����ȴ��̷߳���

	std::atomic_bool isPoolRunning_; // ����״̬

};

#endif

