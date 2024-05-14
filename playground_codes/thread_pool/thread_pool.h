#pragma once

#include <queue>
#include <mutex>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <iostream>
#include <functional>
#include <condition_variable>

using namespace std::chrono_literals;

class ThreadPool {
 public:
  ThreadPool(int thread_num);
  ~ThreadPool() {
    Stop();
  }

  void Stop() {
    stop_.store(true);
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
  }

  template <typename FuncT, typename ...Args>
  void Enqueue(const FuncT& func, Args... args);

 private:
  std::queue<std::function<void()>> task_queue_;
  std::vector<std::thread> workers_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::atomic_bool stop_{false};
  int thread_num_;
};

inline ThreadPool::ThreadPool(int tnum) : thread_num_(tnum) {
  workers_.reserve(tnum);
  for (int i = 0; i < tnum; ++i) {
    workers_.emplace_back(std::thread([this](){
      std::cout << "Thread: " << std::hash<std::thread::id>{}(std::this_thread::get_id()) << " Created\n";
      while (true) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(mtx_);
          cv_.wait(lock, [this](){ return (stop_ || !task_queue_.empty()); });
          if (stop_) return;
          if (task_queue_.empty()) continue;

          task = task_queue_.front();
          task_queue_.pop();
        }
        // Run tasks without lock acquired
        task();
      }
    }));
  }
}

template <typename FuncT, typename ...Args>
void ThreadPool::Enqueue(const FuncT& func, Args... args) {
  auto task = [=]() {
    func(std::forward(args)...);
  };

  {
    std::unique_lock<std::mutex> lock(mtx_);
    std::cout << "Thread: " << std::hash<std::thread::id>{}(std::this_thread::get_id()) << " Enqueue task\n";
    task_queue_.push(std::move(task));
  }
  
  cv_.notify_one();
}
