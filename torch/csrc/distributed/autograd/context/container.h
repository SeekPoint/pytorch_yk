#pragma once

#include <mutex>
#include <unordered_map>

#include <torch/csrc/distributed/autograd/context/context.h>

namespace torch {
namespace distributed {
namespace autograd {
/*
每个worker拥有唯一一个单例DistAutogradContainer，其负责：

对于每一个自动微分过程存储其分布式上下文。
一旦这个自动微分过程结束，就清除其数据。
每个自动微分过程被赋予一个唯一的 autograd_context_id。在每个容器中，这个微分过程的上下文(DistAutogradContext) 依据这个autograd_context_id 来唯一确认。autograd_context_id 是一个 64 bit 的全局唯一id，前 16 bis 是 worker_id，后 48 位是在每个worker内部自动递增id。所以可见，一个Container 之中，是有多个Context的。

此容器还负责维护全局唯一的消息id，用来关联发送/接收自动微分函数对。格式类似于autograd_context_id，是一个64位整数，前16位是工作者id，后48位是worker内部自动递增的。

因为消息 id 和 上下文 id 的前16 位是 worker_id，也就是 rank id，再加上后48位内部自增，所以可以保证 消息 id 和 上下文 id 全局唯一。

3.1 定义
DistAutogradContainer 定义如下，其中：

worker_id_ : 本 worker 的 ID，其实就是本 worker 的 rank。
next_context_id_ ：自增的上下文ID，用来给每个自动微分过程赋予一个唯一的autograd_context_id。在一个传播链条上，其实只有第一个节点的 DistAutogradContainer 用到了 next_context_id_ 来生成 Context，后续节点的 DistAutogradContainer 都是依据第一个 DistAutogradContainer 的 context id 信息来在本地生成对应 context id 的 Context。
next_autograd_message_id_ ：维护全局唯一的消息id，用来关联 发送/接收 自动微分函数对。此变量是在本节点发送时候会使用到。

*/
// Singleton class per worker which is responsible for storing the distributed
// autograd context for each autograd pass and also cleans up data for an
// autograd pass once its done.
//
// Each autograd pass is assigned a unique autograd_context_id and all data for
// that pass (DistAutogradContext) is stored in this container indexed by the
// autograd_context_id. The autograd_context_id itself is a 64 bit globally
// unique id. The first 16 bits is the worker_id and the next 48 bits is an
// auto-incrementing id for each worker.
//
// This container is also responsible for maintaining a globally unique message
// id, which is used to associate send/recv autograd function pairs. The format
// is similar to the autograd_context_id where we have a 64 bit integer with
// first 16 bits being the worker id and next 48 bits are auto-incrementing.
class TORCH_API DistAutogradContainer {
 public:
  explicit DistAutogradContainer(uint32_t num_shards);

  // One time initialization of the container.
  static DistAutogradContainer& init(int64_t worker_id);

  // Retrieve the singleton instance of the container, ensures we have
  // initialized the container.
  static DistAutogradContainer& getInstance();

  // Create a new context for a distributed autograd pass.
  const ContextPtr newContext();

  // Clean up resources for a given context_id once the autograd pass is done.
  // Sends RPC to other workers this worker knows about, telling them to clean
  // up their context as well. Throws an exception if the context_id does not
  // exist.
  void releaseContext(int64_t context_id);

  // Releases an autograd context if it is present on this node. Also sends RPC
  // to other workers this worker knows about, telling them to clean up their
  // context. Does nothing if it is not present.
  void releaseContextIfPresent(int64_t context_id);

  // Checks if the passed in context_id is valid.
  void isValidContext(int64_t context_id);

  // Retrieve the autograd context for a given context_id.
  ContextPtr retrieveContext(int64_t context_id);

  // Retrieves the currently active autograd context for the current thread.
  ContextPtr currentContext();

  // Checks whether or not the current thread has a valid autograd context.
  bool hasValidContext() const;

  // Generate a new autograd_message_id for send/recv autograd functions.
  int64_t newAutogradMessageId();

  // Creates a new autograd context with the provided context_id. If a context
  // already exists with the provided context_id, we just return it.
  // This does not set the current context for the current thread.
  ContextPtr getOrCreateContext(int64_t context_id);

  // Retrieves the maximum possible autograd_context_id/autograd_message_id that
  // can be generated by this worker.
  int64_t getMaxId();

  // Retrieves the worker ID for this node
  rpc::worker_id_t getWorkerId() const;

  // Can set current context id if there is no valid context yet
  static void setCurrentContextId(int64_t contextId);

  // Forcibly sets the thread local current context id. Should only be used in
  // cases where you know what you're doing and need to override the thread
  // local. Otherwise, use setCurrentContextId instead.
  static void forceCurrentContextId(int64_t contextId);

  // Clear current context id
  void clearCurrentContext();

  // Returns the number of autograd contexts in the container.
  size_t numAutogradContexts() const;

  // Returns the current thread local context id for this thread.
  static int64_t currentContextId();

  DistAutogradContainer(const DistAutogradContainer&) = delete;
  DistAutogradContainer& operator=(const DistAutogradContainer&) = delete;
  DistAutogradContainer(DistAutogradContainer&&) = delete;
  DistAutogradContainer& operator=(DistAutogradContainer&&) = delete;

 private:
  // Number of shards for the map storing autograd contexts. We'd like this
  // to be a power of 2 and we don't expect a value much higher than the
  // number of cores would provide much benefit.
  static constexpr uint32_t kNumDefaultShards = 128;

  // Use cache line size for alignment.
  static constexpr int kCacheLineSize = 64;

  // Structure holding one shard of the sharded autograd context map with its
  // associated lock. Align to cache line size to avoid contention between
  // adjacent entries.
  struct alignas(kCacheLineSize) ContextsShard {
    // Lock for this shard.
    mutable std::mutex lock;

    // Map storing autograd contexts for this shard.
    std::unordered_map<int64_t, ContextPtr> contexts; // 这里存储了上下文指针
  };

  DistAutogradContainer() = delete;
  ~DistAutogradContainer() = default;

  static DistAutogradContainer& getInstanceInternal();

  // Retrieve the shard for given context_id.
  ContextsShard& getShard(int64_t context_id);

  // Sends an RPC to the workers that have a context corresponding to passed in
  // context_id. This function should be called with the lock.
  void sendReleaseContextRpc(
      const std::unordered_set<rpc::worker_id_t>& workerIds,
      int64_t context_id);

  // Erase context_id from the autograd context map, and reset the thread local
  // current context id if it corresponds to the passed in context id. This
  // function should be called with the lock.
  void eraseContextIdAndReset(ContextsShard& shard, int64_t context_id);

  // Compute the number of shards for the autograd_contexts_ map.
  static uint32_t computeNumShards();

  // Auto incrementing context id used to identify unique autograd passes.
  // Initialized with the first 16 bits being the worker_id.
  std::atomic<int64_t> next_context_id_; // 新增上下文id

  // Unique id to identify a worker in the distributed setting.
  int16_t worker_id_;

  // Whether or not the container has been initialized appropriately.
  bool initialized_;

  // Sharded autograd context map.
  std::vector<ContextsShard> autograd_contexts_; // 存储上下文列表

  // Number of shards for the sharded autograd_contexts_ map.
  uint32_t num_shards_;

  // Autograd message id to identify unique send/recv autograd function pairs.
  std::atomic<int64_t> next_autograd_message_id_;

  // Maximum allowed value for autograd_context_id or autograd_message_id.
  int64_t max_id_;
};

} // namespace autograd
} // namespace distributed
} // namespace torch
