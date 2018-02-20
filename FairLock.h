#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cassert>

/// @brief a read-write lock that ensures fairness
///
/// this lock should prevent reader and writer starvation. neither
/// reads nor writes are preferred, but executed in turns
/// 
/// operations of the same type (i.e. reads or writes) that try to 
/// acquire the lock but are blocked are queued in FIFO order
///
/// write operations are exclusive, but multiple read operations can
/// be executed together, provided that no further writers are queued
///
/// write operations will only be able to acquire the lock if there are
/// no other writers or readers currently holding the lock, and if there
/// are no queued "preferred" readers
/// once a write operation successfully acquires the lock, it will set
/// the preferred next mode to "read"
///
/// read operations will be able to acquire the lock if there is no
/// writer currently holding the lock, and if there are no queued 
/// "preferred" writers
/// once a read operation successfully acquires the lock, it will set
/// the preferred next mode to "write"
class FairLock {
 private:
  /// @brief an object that can be notified and that is linked with
  /// its followers in a singly linked list
  struct Notifiable {
    Notifiable() : next(nullptr) {}

    /// @brief condition variable, used to notifiy the object
    std::condition_variable cond;
    
    /// @brief next following object in singly linked list
    Notifiable* next;
  };

  /// @brief a simple freelist for reusing heap-allocated Notifiable objects 
  class Freelist {
   public:
    /// @brief creates an empty freelist
    Freelist() 
      : _head(nullptr) {}

    /// @brief destroys all elements on the freelist
    ~Freelist() {
      while (_head != nullptr) {
        Notifiable* notifiable = _head;
        _head = _head->next;
        delete notifiable;
      }
    }

    /// @brief produces a new Notifiable, either from freelist or 
    /// by allocating a new object on the heap
    Notifiable* pop() {
      Notifiable* notifiable = _head;
      if (notifiable != nullptr) {
        // some reusable object found on the freelist
        _head = _head->next;
        notifiable->next = nullptr;
        return notifiable;
      }
      // freelist is empty
      return new Notifiable();
    }
        
    /// @brief adds a notifiable back to the freelist
    void push(Notifiable* notifiable) noexcept {
      assert(notifiable != nullptr);
      notifiable->next = _head;
      _head = notifiable;
    }

   private:
    /// @brief freelist head
    Notifiable* _head;
  };
  
  /// @brief a simple queue for readers or writers
  class Queue {
   public:
    /// @brief creates an empty queue
    Queue() 
      : _start(nullptr), _end(nullptr) {}

    /// @brief whether or not the queue contains elements
    bool empty() const { return _start == nullptr; }
    
    /// @brief wake up the queue head if the queue is non-empty
    /// returns true if the queue head was woken up, false otherwise
    bool notify() {
      if (empty()) {
        // queue is empty, nothing to do
        return false;
      }
      // wake up queue head
      _start->cond.notify_one();
      return true;
    }
  
    /// @brief adds the Notifiable to the queue
    void add(Notifiable* notifiable) noexcept {
      if (empty()) {
        // queue is empty
        assert(_end == nullptr);
        _start = notifiable;
      } else {
        // queue is not empty
        assert(_end != nullptr);
        _end->next = notifiable;
      }
      _end = notifiable;
    }
  
    /// @brief removes a notifiable from the queue
    /// asserts that the notifiable exists in the queue
    void remove(Notifiable* notifiable) noexcept {
      // element is at start of the queue
      if (_start == notifiable) {
        _start = _start->next;
        if (_start == nullptr) {
          // queue is now empty
          _end = nullptr;
          assert(empty());
        }
        return;
      }

      // element was not at start of queue. now find it
      Notifiable* n = _start;
      while (n != nullptr) {
        if (n->next == notifiable) {
          n->next = notifiable->next;
          if (n->next == nullptr) {
            _end = n;
          }
          return;
        }
        n = n->next;
      }
      assert(false);
    }

   private:
    /// @brief queue head
    Notifiable* _start;
    
    /// @brief queue tail
    Notifiable* _end;
  };
  
  /// @brief type for next preferred operation phase
  enum class Phase : int32_t {
    READ,
    WRITE
  };
  
  typedef std::chrono::duration<double> duration_t;
  typedef std::chrono::time_point<std::chrono::steady_clock, duration_t> timepoint_t;

 public:
  FairLock(FairLock const&) = delete;
  FairLock& operator=(FairLock const&) = delete;

  /// @brief creates a new lock instance
  FairLock() 
    : _whoEntered(0), 
      _nextPreferredPhase(Phase::READ) {}
 
  /// @brief tries to lock for writing, with timeout 
  bool tryWriteLock(double timeout) {
    std::unique_lock<std::mutex> guard(_mutex);

    if (_whoEntered != 0 ||
        (!_readQueue.empty() && _nextPreferredPhase == Phase::READ)) {
      // someone has acquired the lock already, or noone has acquired the lock,
      // but it is a reader's turn now
      timepoint_t now = _clock.now();
      timepoint_t deadline = now + duration_t(timeout);
     
      // put a Notifiable object into the write queue, that can be woken up whenever
      // it is its turn 
      Notifiable* notifiable = _freelist.pop();
      _writeQueue.add(notifiable);

      bool expired = !notifiable->cond.wait_until(guard, deadline, [&]() -> bool { 
        return _whoEntered == 0 && (_readQueue.empty() || _nextPreferredPhase == Phase::WRITE); 
      });
     
      // clean up write queue
      _writeQueue.remove(notifiable); 
      // recycle Notifiable
      _freelist.push(notifiable); 

      if (expired) {
        // could not acquire write-lock in time
        return false;
      }
    }

    // successfully acquired the write-lock
    assert(_whoEntered == 0);
    _whoEntered = -1; // a writer
    _nextPreferredPhase = Phase::READ;
    return true;
  }
  
  /// @brief locks for writing
  void writeLock() {
    std::unique_lock<std::mutex> guard(_mutex);

    if (_whoEntered != 0 ||
        (!_readQueue.empty() && _nextPreferredPhase == Phase::READ)) {
      // someone has acquired the lock already, or noone has acquired the lock,
      // but it is a reader's turn now
      
      // put a Notifiable object into the write queue, that can be woken up whenever
      // it is its turn 
      Notifiable* notifiable = _freelist.pop();
      _writeQueue.add(notifiable);

      notifiable->cond.wait(guard, [&]() -> bool { 
        return _whoEntered == 0 && (_readQueue.empty() || _nextPreferredPhase == Phase::WRITE); 
      });

      // clean up write queue
      _writeQueue.remove(notifiable);
      // recycle Notifiable
      _freelist.push(notifiable); 
    }

    // successfully acquired the write-lock
    assert(_whoEntered == 0);
    _whoEntered = -1; // a writer
    _nextPreferredPhase = Phase::READ;
  }
  
  /// @brief releases the write-lock
  void unlockWrite() {
    std::unique_lock<std::mutex> guard(_mutex);

    // when we get here, exactly one writer must have acquired the lock
    assert(_whoEntered == - 1);
    _whoEntered = 0;

    // wake up potential other waiters
    if (_nextPreferredPhase == Phase::READ) {
      if (!_readQueue.notify()) {
        _writeQueue.notify();
      }
    } else if (_nextPreferredPhase == Phase::WRITE) {
      if (!_writeQueue.notify()) {
        _readQueue.notify();
      }
    }
  }
      
  /// @brief tries to lock for reading, with timeout 
  bool tryReadLock(double timeout) {
    std::unique_lock<std::mutex> guard(_mutex);

    if (_whoEntered == -1 ||
        (!_writeQueue.empty() && _nextPreferredPhase == Phase::WRITE)) {
      // a writer has acquired the lock already, or no writer has acquired the lock yet,
      // but it is a writer's turn now
      timepoint_t now = _clock.now();
      timepoint_t deadline = now + duration_t(timeout);
      
      // put a Notifiable object into the write queue, that can be woken up whenever
      // it is its turn 
      Notifiable* notifiable = _freelist.pop();
      _readQueue.add(notifiable);
      
      bool expired = !notifiable->cond.wait_until(guard, deadline, [&]() -> bool { 
        return _whoEntered >= 0 && (_writeQueue.empty() || _nextPreferredPhase == Phase::READ); 
      });
      
      // clean up read queue
      _readQueue.remove(notifiable);
      // recycle Notifiable
      _freelist.push(notifiable); 

      if (expired) {
        // could not acquire read-lock in time
        return false;
      }
    }

    // successfully acquired the read-lock
    assert(_whoEntered >= 0);
    ++_whoEntered;
    _nextPreferredPhase = Phase::WRITE;
    return true;
  }
  
  /// @brief locks for reading
  void readLock() {
    std::unique_lock<std::mutex> guard(_mutex);

    if (_whoEntered == -1 || 
        (!_writeQueue.empty() && _nextPreferredPhase == Phase::WRITE)) {
      // a writer has acquired the lock already, or no writer has acquired the lock yet,
      // but it is a writer's turn now

      // put a Notifiable object into the write queue, that can be woken up whenever
      // it is its turn 
      Notifiable* notifiable = _freelist.pop();
      _readQueue.add(notifiable);

      notifiable->cond.wait(guard, [&]() -> bool { 
        return _whoEntered >= 0 && (_writeQueue.empty() || _nextPreferredPhase == Phase::READ); 
      });

      // clean up read queue
      _readQueue.remove(notifiable);
      // recycle Notifiable
      _freelist.push(notifiable); 
    }

    // successfully acquired the read-lock
    assert(_whoEntered >= 0);
    ++_whoEntered;
    _nextPreferredPhase = Phase::WRITE;
  }

  /// @brief releases the read-lock
  void unlockRead() {
    std::unique_lock<std::mutex> guard(_mutex);

    // when we get here, there must have been at least one reader having
    // acquired the lock, but no writers    
    assert(_whoEntered > 0);
    --_whoEntered;
    
    // wake up potential other waiters
    if (_nextPreferredPhase == Phase::READ) {
      if (!_readQueue.notify()) {
        _writeQueue.notify();
      }
    } else if (_nextPreferredPhase == Phase::WRITE) {
      if (!_writeQueue.notify()) {
        _readQueue.notify();
      }
    }
  }

 private:
  /// @brief mutex protecting the entire data structure
  std::mutex _mutex;

  /// @brief currently queued write operations that will be notified eventually
  Queue _writeQueue;
  
  /// @brief currently queued read operations that will be notified eventually
  Queue _readQueue;
  
  /// @brief a freelist for reusing heap-allocated objects
  Freelist _freelist;
  
  // @brief who is currently holding the lock:
  //    -1 = a writer got the lock, 
  //     0 = noone got the lock
  //   > 0 = # readers got the lock
  int32_t _whoEntered; 

  /// @brief next phase the lock will grant access to 
  /// when there are both readers and writers queued
  Phase _nextPreferredPhase;
  
  static std::chrono::steady_clock _clock;
};
