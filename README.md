# threadpool

基于C++实现的线程池，通过submitTask() 接口调用，自动分配线程完成任务。
  - submitTask() 基于可变参模板和引用折叠原理实现，支持任意任务函数和任意数量参数的传递。
  - 模式可设定：
        fixed 固定线程数量，默认值基于CPU核心数量设定。
        cached 动态增减，任务堆积超过一定时间可在最大线程数量下动态创建线程，提高处理任务的速度。默认数量外的空闲线程等待时间超时则自动销毁。
  - threadpool_fin 使用future类型接收返回值，threadpool 自定义result, any, semaphore 实现接收返回值
  - 使用条件变量(condition_variable) 和互斥量(mutex) 实现线程间的通信，保证线程安全。
  - 使用unordered_map 和queue 容器分别管理线程池内的线程和用户提交的任务。
  - GC机制，保证线程池析构时，所有线程池创建的线程也被析构。

C++ Based threadpool
  	- Designed and developed thread pool which can be used to boost efficiency when dealing high concurrent network service or time-consuming tasks.
    - Based on variadic templates and reference collapse to determine submitTask() function for user to submit any task function with any parameters.
    - Determined fixed mode and cached mode which allows the thread pool to increase or decrease threads dynamically. 
    - Accomplished communication between main thread and executive thread with condition variable and mutex.
    - Determined garbage collection process to free the memory resources when thread pool is destructed.
