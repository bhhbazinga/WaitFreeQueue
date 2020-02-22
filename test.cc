#include <chrono>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "waitfree_queue.h"

int main(int argc, char const* argv[]) {
  (void)argc;
  (void)argv;

  int thread_nums = 8;
  int elements = 10000;
  WaitFreeQueue<int> q(thread_nums);
  std::vector<std::thread> threads;
  std::atomic<bool> go = false;
  for (int i = 0; i < thread_nums; ++i) {
    threads.push_back(std::thread([&] {
      while (!go) {
        std::this_thread::yield();
      }

      for (int i = 0; i < elements / thread_nums; ++i) {
        q.Enqueue(i);
      }
    }));
  }

  go = true;
  auto t1_ = std::chrono::steady_clock::now();
  for (auto& t : threads) {
    t.join();
  }
  auto t2_ = std::chrono::steady_clock::now();
  int ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t2_ - t1_).count();
  std::cout << "time:" << ms << "\n";
}