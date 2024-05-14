#include <unistd.h>
#include "thread_pool.h"

int main() {
  ThreadPool th_pool(5);
  sleep(1);
  for (int i = 0; i < 15; ++i) {
    th_pool.Enqueue([i](){
      std::this_thread::sleep_for(1000ms);
      std::cout << "Thread " << std::hash<std::thread::id>{}(std::this_thread::get_id())
                << " Running task " << i << " in Thread Pool\n";
    });
  }
  sleep(5);
  th_pool.Stop();
  return 1;
}
