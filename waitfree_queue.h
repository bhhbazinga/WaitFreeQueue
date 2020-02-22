#ifndef WAITFREE_QUEUE_H
#define WAITFREE_QUEUE_H

#include <atomic>
#include <cassert>
// #include <queue>

#include "HazardPointer/reclaimer.h"

template <typename T>
class QueueReclaimer;

template <typename T>
class WaitFreeQueue {
  struct Node;
  struct DummyNode;
  struct RegularNode;
  struct State;

  typedef int ThreadId;
  typedef std::atomic<State*> AtomicState;

  friend QueueReclaimer<T>;

 public:
  WaitFreeQueue(int max_threads)
      : head_(new DummyNode()),
        tail_(head_.load(std::memory_order_relaxed)),
        phase_(0),
        atomic_states_(new AtomicState[max_threads]),
        size_(0),
        max_threads_(max_threads) {
    for (int i = 0; i < max_threads; ++i) {
      atomic_states_[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ~WaitFreeQueue() {}

  WaitFreeQueue(const WaitFreeQueue& other) = delete;
  WaitFreeQueue(WaitFreeQueue&& other) = delete;
  WaitFreeQueue& operator=(const WaitFreeQueue& other) = delete;
  WaitFreeQueue& operator=(WaitFreeQueue&& other) = delete;

  template <typename... Args>
  void Emplace(Args&&... args);

  void Enqueue(const T& value) {
    static_assert(std::is_copy_constructible<T>::value,
                  "T must be copy constructible");
    Emplace(value);
  };

  void Enqueue(T&& value) {
    static_assert(std::is_constructible_v<T, T&&>,
                  "T must be constructible with T&&");
    Emplace(std::forward<T>(value));
  }

  bool Dequeue(T& value);

  size_t size() const { return size_.load(std::memory_order_consume); }

 private:
  // If already set thread_id then return it, else scan states Linearly when
  // CAS succeed then return the index of states.
  ThreadId get_thread_id();

  Node* get_head() const { return head_.load(std::memory_order_acquire); }
  Node* get_tail() const { return tail_.load(std::memory_order_acquire); }

  State* get_state(ThreadId tid) {
    return atomic_states_[tid].load(std::memory_order_consume);
  }

  void set_state(ThreadId tid, State* new_state) {
    State* old_state =
        atomic_states_[tid].exchange(new_state, std::memory_order_release);
    auto& reclaimer = QueueReclaimer<T>::GetInstance();
    reclaimer.ReclaimLater(old_state,
                           [](void* ptr) { delete static_cast<State*>(ptr); });
    reclaimer.ReclaimNoHazardPointer();
  }

  bool test_set_state(ThreadId tid, State* expected, State* desired) {
    if (atomic_states_[tid].compare_exchange_strong(
            expected, desired, std::memory_order_release)) {
      auto& reclaimer = QueueReclaimer<T>::GetInstance();
      reclaimer.ReclaimLater(
          expected, [](void* ptr) { delete static_cast<State*>(ptr); });
      reclaimer.ReclaimNoHazardPointer();
      return true;
    } else {
      delete desired;
      return false;
    }
  }

  long get_max_phase() {
    return phase_.fetch_add(1l, std::memory_order_relaxed);
  }

  void Help(long phase);
  void HelpEnqueue(ThreadId tid, long phase);
  void HelpDequeue(ThreadId tid, long phase);
  void HelpFinishEnqueue();
  void HelpFinishDequeue();

  bool StillPending(ThreadId tid, long phase) {
    HazardPointer hp;
    State* state = AcquireSafeState(tid, hp);
    bool flag = state->pending && state->phase <= phase;
    return flag;
  }

  // Get state and mark it as hazard.
  State* AcquireSafeState(ThreadId tid, HazardPointer& hp);
  // Get safe node and its next, ensure next is the succeed of node
  // and both pointer are safety.
  // REQUIRE: atomic_node is head_ or tail_.
  void AcquireSafeNodeAndNext(std::atomic<Node*>& atomic_node, Node** node_ptr,
                              Node** next_ptr, HazardPointer& node_hp,
                              HazardPointer& next_hp);
  // Get node and mark it as hazard.
  Node* AcquireSafeNode(std::atomic<Node*>& atomic_node, HazardPointer& hp);

  void UnMarkHazard(int index) {
    auto& reclaimer = QueueReclaimer<T>::GetInstance();
    reclaimer.UnMarkHazard(index);
  }

  struct Node {
    Node() : dequeue_id(-1), next(nullptr) {}
    virtual ~Node() = default;

    virtual void Release() = 0;

    Node* get_next() const { return next.load(std::memory_order_acquire); }
    ThreadId get_dequeue_id() const {
      return dequeue_id.load(std::memory_order_consume);
    }
    virtual bool dummy() { return false; }

    std::atomic<ThreadId> dequeue_id;
    std::atomic<Node*> next;
  };

  struct DummyNode : Node {
    DummyNode() = default;
    ~DummyNode() override = default;

    void Release() override { delete this; }

    bool dummy() override { return true; }
  };

  struct RegularNode : Node {
    template <typename... Args>
    explicit RegularNode(ThreadId enqueue_id_, Args&&... args)
        : enqueue_id(enqueue_id_), value(std::forward<Args>(args)...) {}

    void Release() override { delete this; }

    bool dummy() override { return false; }
    ThreadId enqueue_id;
    T value;
  };

  struct State {
    State() : pending(false), enqueue(false), phase(-1), node(nullptr) {}

    State(bool pending_, bool enqueue_, long phase_, Node* node_)
        : pending(pending_), enqueue(enqueue_), phase(phase_), node(node_) {}

    State(bool pending_, bool enqueue_, long phase_, Node* node_,
          const T& value_)
        : pending(pending_),
          enqueue(enqueue_),
          phase(phase_),
          node(node_),
          value(value_) {}

    ~State() {}

    bool pending;
    bool enqueue;
    long phase;
    Node* node;
    T value;
  };

  static Reclaimer::HazardPointerList& get_global_hp_list() {
    static Reclaimer::HazardPointerList hp_list;
    return hp_list;
  }

  // State* NewState() {
  //   // if (node_pool_)
  // }

  // State* NewNode() {}

  std::atomic<Node*> head_;
  std::atomic<Node*> tail_;
  std::atomic<long> phase_;
  std::atomic<State*>* atomic_states_;
  std::atomic<size_t> size_;
  int max_threads_;

  static Reclaimer::HazardPointerList global_hp_list_;
  // static thread_local std::queue<State*> state_pool_;
  // static thread_local std::queue<Node*> node_pool_;
};

// template <typename T>
// thread_local std::queue<typename WaitFreeQueue<T>::State*>
// WaitFreeQueue<T>::state_pool_;

// template <typename T>
// thread_local std::queue<typename WaitFreeQueue<T>::Node*>
// WaitFreeQueue<T>::node_pool_;

template <typename T>
Reclaimer::HazardPointerList WaitFreeQueue<T>::global_hp_list_;

template <typename T>
class QueueReclaimer : public Reclaimer {
  friend WaitFreeQueue<T>;

 private:
  QueueReclaimer(HazardPointerList& hp_list) : Reclaimer(hp_list) {}
  ~QueueReclaimer() override = default;

  static QueueReclaimer<T>& GetInstance() {
    thread_local static QueueReclaimer reclaimer(
        WaitFreeQueue<T>::global_hp_list_);
    return reclaimer;
  }
};

template <typename T>
typename WaitFreeQueue<T>::State* WaitFreeQueue<T>::AcquireSafeState(
    ThreadId tid, HazardPointer& hp) {
  auto& reclaimer = QueueReclaimer<T>::GetInstance();
  State* state = get_state(tid);
  State* temp;
  do {
    hp.UnMark();
    temp = state;
    hp = HazardPointer(&reclaimer, state);
    state = get_state(tid);
  } while (state != temp);
  return state;
}

template <typename T>
void WaitFreeQueue<T>::AcquireSafeNodeAndNext(std::atomic<Node*>& atomic_node,
                                              Node** node_ptr, Node** next_ptr,
                                              HazardPointer& node_hp,
                                              HazardPointer& next_hp) {
  Node* node = atomic_node.load(std::memory_order_acquire);
  Node* next;
  Node* temp_node;
  Node* temp_next;
  auto& reclaimer = QueueReclaimer<T>::GetInstance();
  do {
    do {
      // 1.UnMark old node;
      node_hp.UnMark();
      temp_node = node;
      // 2. Mark node.
      node_hp = HazardPointer(&reclaimer, node);
      node = atomic_node.load(std::memory_order_acquire);
      // 3. Make sure the node is still the one we mark before.
    } while (temp_node != node);
    // 4. UnMark old next.
    next_hp.UnMark();
    next = node->get_next();
    temp_next = next;
    // 5. Mark next.
    next_hp = HazardPointer(&reclaimer, next);
    next = node->get_next();
    // 6. Make sure the next is still the succeed of first.
  } while (temp_next != next);

  *node_ptr = node;
  *next_ptr = next;
}

template <typename T>
typename WaitFreeQueue<T>::Node* WaitFreeQueue<T>::AcquireSafeNode(
    std::atomic<Node*>& atomic_node, HazardPointer& hp) {
  auto& reclaimer = QueueReclaimer<T>::GetInstance();
  Node* node = atomic_node.load(std::memory_order_consume);
  Node* temp;
  do {
    hp.UnMark();
    temp = node;
    hp = HazardPointer(&reclaimer, node);
  } while (node != temp);
  return node;
}

template <typename T>
typename WaitFreeQueue<T>::ThreadId WaitFreeQueue<T>::get_thread_id() {
  static thread_local ThreadId thread_id = -1;
  if (thread_id != -1) return thread_id;

  State* state = new State();
  for (int i = 0; i < max_threads_; ++i) {
    State* expected = nullptr;
    if (atomic_states_[i].compare_exchange_strong(expected, state,
                                                  std::memory_order_release)) {
      thread_id = i;
      break;
    }
  }

  assert(thread_id != -1);  // If assert fired, you should change max_threads.
  assert(thread_id < max_threads_);
  return thread_id;
}

template <typename T>
template <typename... Args>
void WaitFreeQueue<T>::Emplace(Args&&... args) {
  static_assert(std::is_constructible_v<T, Args&&...>,
                "T must be constructible with Args&&...");

  ThreadId tid = get_thread_id();
  long phase = get_max_phase();
  RegularNode* new_node = new RegularNode(tid, std::forward<Args>(args)...);
  set_state(tid, new State(true, true, phase, new_node));
  Help(phase);
  HelpFinishEnqueue();
}

template <typename T>

bool WaitFreeQueue<T>::Dequeue(T& value) {
  ThreadId tid = get_thread_id();
  long phase = get_max_phase();
  set_state(tid, new State(true, false, phase, nullptr));
  Help(phase);
  HelpFinishDequeue();

  State* state = get_state(tid);
  // At this point, node may be deleted.
  Node* node = state->node;
  if (nullptr == node) return false;

  value = std::move(state->value);
  auto& reclaimer = QueueReclaimer<T>::GetInstance();
  reclaimer.ReclaimLater(node,
                         [](void* ptr) { static_cast<Node*>(ptr)->Release(); });
  reclaimer.ReclaimNoHazardPointer();
  return true;
}

template <typename T>
void WaitFreeQueue<T>::Help(long phase) {
  for (int i = 0; i < max_threads_; ++i) {
    HazardPointer hp;
    State* state = AcquireSafeState(i, hp);
    if (nullptr == state) break;

    if (state->pending && state->phase <= phase) {
      if (state->enqueue) {
        HelpEnqueue(i, phase);
      } else {
        HelpDequeue(i, phase);
      }
    }
  }
}

template <typename T>
void WaitFreeQueue<T>::HelpEnqueue(ThreadId tid, long phase) {
  while (StillPending(tid, phase)) {
    HazardPointer last_hp, next_hp;
    Node* last = AcquireSafeNode(tail_, last_hp);
    Node* next = last->get_next();
    if (last != get_tail()) continue;

    if (nullptr == next) {
      if (!StillPending(tid, phase)) return;
      HazardPointer hp;
      State* state = AcquireSafeState(tid, hp);
      if (last->next.compare_exchange_strong(next, state->node,
                                             std::memory_order_release)) {
        HelpFinishEnqueue();
        return;
      }
    }
  }
}

template <typename T>
void WaitFreeQueue<T>::HelpFinishEnqueue() {
  Node* last;
  Node* next;
  HazardPointer last_hp, next_hp;
  AcquireSafeNodeAndNext(tail_, &last, &next, last_hp, next_hp);
  if (next == nullptr) return;

  int tid = static_cast<RegularNode*>(next)->enqueue_id;
  HazardPointer state_hp;
  State* cur_state = AcquireSafeState(tid, state_hp);
  if (last == get_tail() && cur_state->node == next) {
    test_set_state(tid, cur_state,
                   new State(false, true, cur_state->phase, next));
    if (tail_.compare_exchange_strong(last, next, std::memory_order_release)) {
      size_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

template <typename T>
void WaitFreeQueue<T>::HelpDequeue(ThreadId tid, long phase) {
  while (StillPending(tid, phase)) {
    Node* first;
    Node* next;
    HazardPointer first_hp, next_hp;
    AcquireSafeNodeAndNext(head_, &first, &next, first_hp, next_hp);
    Node* last = get_tail();
    if (first != get_head()) continue;

    if (first == last) {      // Queue might be empty.
      if (nullptr == next) {  // Queue is empty.
        HazardPointer state_hp;
        State* cur_state = AcquireSafeState(tid, state_hp);
        if (last == get_tail() && StillPending(tid, phase)) {
          test_set_state(tid, cur_state,
                         new State(false, false, cur_state->phase, nullptr));
        }
      } else {  // Help other thread finish enqueue.
        HelpFinishEnqueue();
      }
    } else {
      HazardPointer state_hp;
      State* cur_state = AcquireSafeState(tid, state_hp);
      Node* node = cur_state->node;
      if (!StillPending(tid, phase)) return;

      if (first == get_head() && node != first) {
        State* new_state = new State(true, false, cur_state->phase, first,
                                     static_cast<RegularNode*>(next)->value);
        if (!test_set_state(tid, cur_state, new_state)) {
          continue;
        }
      }

      int expected = -1;
      first->dequeue_id.compare_exchange_strong(expected, tid,
                                                std::memory_order_release);
      HelpFinishDequeue();
    }
  }
}

template <typename T>
void WaitFreeQueue<T>::HelpFinishDequeue() {
  Node* first;
  Node* next;
  HazardPointer first_hp, next_hp;
  AcquireSafeNodeAndNext(head_, &first, &next, first_hp, next_hp);
  int tid = first->get_dequeue_id();
  if (tid == -1) return;

  HazardPointer state_hp;
  State* cur_state = AcquireSafeState(tid, state_hp);
  if (first == get_head() && next != nullptr) {
    test_set_state(tid, cur_state,
                   new State(false, false, cur_state->phase, cur_state->node,
                             cur_state->value));
    if (head_.compare_exchange_strong(first, next)) {
      size_.fetch_sub(1, std::memory_order_release);
    }
  }
}

#endif
