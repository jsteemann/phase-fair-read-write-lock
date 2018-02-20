#include <thread>
#include <vector>
#include <iostream>
#include <functional>

#include "FairLock.h"

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " iterations concurrency" << std::endl;
    std::abort();
  }

  int const count = std::stoll(argv[1]);
  int const concurrency = std::stoll(argv[2]);
    
  std::mutex mutex;
  std::condition_variable start;
  bool started = false;
  
  std::cout << "ITERATIONS: " << count << ", WRITE CONCURRENCY: " << concurrency << std::endl;
  
  FairLock lock;
  int value = 0;
  
  std::cout << "VALUE AT START IS: " << value << std::endl;

  std::vector<std::thread> threads;
  threads.reserve(20);
  
  auto funcTryWrite = [&lock, &value, &mutex, &start, &started](int id, int iterations) {
    {
      std::unique_lock<std::mutex> guard(mutex);
      start.wait(guard, [&]() -> bool { return started; });
      std::cout << "WRITER #" << id << " STARTED!\n";
    }
    
    for (int i = 0; i < iterations; ++i) {
      while (!lock.tryWriteLock(100)) {}
      std::cout << "WRITER #" << id << " IN LOCK\n";
      ++value;
      lock.unlockWrite();
    }
    std::cout << "WRITER #" << id << " DONE!\n";
  };

  auto funcWrite = [&lock, &value, &mutex, &start, &started](int id, int iterations) {
    {
      std::unique_lock<std::mutex> guard(mutex);
      start.wait(guard, [&]() -> bool { return started; });
      std::cout << "WRITER #" << id << " STARTED!\n";
    }
    
    for (int i = 0; i < iterations; ++i) {
      lock.writeLock();
      std::cout << "WRITER #" << id << " IN LOCK\n";
      ++value;
      lock.unlockWrite();
    }
    std::cout << "WRITER #" << id << " DONE!\n";
  };
  
  auto funcTryRead = [&lock, &value, &mutex, &start, &started](int id, int iterations) {
    {
      std::unique_lock<std::mutex> guard(mutex);
      start.wait(guard, [&]() -> bool { return started; });
      std::cout << "READER #" << id << " STARTED!\n";
    }

    for (int i = 0; i < iterations; ++i) {
      while (!lock.tryReadLock(100)) {}
      std::cout << "READER #" << id << " IN LOCK\n";
      lock.unlockRead();
    }
    std::cout << "READER #" << id << " DONE!\n";
  };
  
  auto funcRead = [&lock, &value, &mutex, &start, &started](int id, int iterations) {
    {
      std::unique_lock<std::mutex> guard(mutex);
      start.wait(guard, [&]() -> bool { return started; });
      std::cout << "READER #" << id << " STARTED!\n";
    }

    for (int i = 0; i < iterations; ++i) {
      lock.readLock();
      std::cout << "READER #" << id << " IN LOCK\n";
      lock.unlockRead();
    }
    std::cout << "READER #" << id << " DONE!\n";
  };
 
  for (int i = 0; i < concurrency; ++i) { 
    switch (i % 4) {
      case 0:
        threads.emplace_back(funcWrite, i, count);
        break;
      case 1:
        threads.emplace_back(funcRead, i, count);
        break;
      case 2:
        threads.emplace_back(funcTryWrite, i, count);
        break;
      case 3:
        threads.emplace_back(funcTryRead, i, count);
        break;
    } 
  }
  
  {
    std::unique_lock<std::mutex> guard(mutex);
    started = true;
  }
  start.notify_all();
  
  for (auto& it : threads) {
    it.join();
  }

  std::cout << "VALUE AT END IS: " << value << std::endl;
}

