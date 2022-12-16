---
title: 同步语义(Synchronize-with)和Acquire-Release Fence
date: 2022-12-07
author: Quan
tag: [Concurrency, C++, Memory Barrier]
category: C++ Low-level Concurrency
---

> This article is partly referred to & translated from [Jeff Preshing’s blog](https://preshing.com/20130922/acquire-and-release-fences/). Personal thoughts added.

在第一篇[技术博客](https://chocho-quan.github.io/2022/11/01/dclp/)里，我们就提到了同步语义(Synchronize-with)的重要性。问题在于，在多线程的环境下，谁也不知道处理器实际执行程序的顺序如何。可是我们程序的正确执行，往往需要一个合理的顺序，例如第一个线程需要读共享变量`B`，然而第二个线程会修改`B`。我们需要告诉处理器，到底谁是先执行的那一个。

而同步语义就是这里的屏障，它保证了某个线程的某些内存操作(甚至可能是non-atomic的)对其他的某个线程的某些操作来说一定可见。

这篇文章想讲明白实现同步最重要的方法之一 ———— Acquire & Release Fence。

在正式开始之前，我们需要知道在同步语义里，一般都能找到两个最关键的变量，分别是guard variable和payload。其中payload是线程间共享的变量，而guard variable人如其名，保护对payload的访问权限。现在我们可以开始来看大名鼎鼎的Acquire & Release Fence了。

## 理解Acquire & Release Fence

直接的表达两者的作用即
> Acquire Fence防止重排任何屏障之前的读操作 & 屏障之后的读写操作。
> Release Fence防止重排任何屏障之前的读写操作 & 屏障之后的写操作。

用我们在[内存屏障的博客](https://chocho-quan.github.io/2022/11/21/understanding-memory-barrier/)里讲的概念来说，Acquire Fence就相当于#LoadLoad + #LoadStore屏障，而Release Fence则等同于#StoreStore + #LoadStore屏障。

接下来我们通过一个经典的例子来理解Acquire & Release Fence。假设我们通过`Message`类在多个线程间进行通信，用`g_payload`和`g_guard`来表示这个同步语义中的payload与guard variable

```cpp
struct Message {
    clock_t tick;
    const char* str;
    void* param;
};

Message g_payload;
std::atomic<int> g_guard(0);
```

通信的两个函数分别如下

```cpp
void SendTestMessage(void* param) {
    // Copy to shared memory using non-atomic stores.
    g_payload.tick  = clock();
    g_payload.str   = "TestMessage";
    g_payload.param = param;
    
    // Release fence.
    std::atomic_thread_fence(std::memory_order_release);

    // Perform an atomic write to indicate that the message is ready.
    g_guard.store(1, std::memory_order_relaxed);
}

bool TryReceiveMessage(Message& result) {
    // Perform an atomic read to check whether the message is ready.
    int ready = g_guard.load(std::memory_order_relaxed);
    if (ready != 0) {
        // Acquire fence.
        std::atomic_thread_fence(std::memory_order_acquire);
        // Yes. Copy from shared memory using non-atomic loads.
        result.tick  = g_payload.tick;
        result.str   = g_payload.str;
        result.param = g_payload.param;
        return true;
    }
    // No.
    return false;
}
```

当`g_guard`在`TryReceiveMessage`函数中load为1时，Acquire Fence会启动，从而保证了同步语义。我们假设线程I运行`SendTestMessage`而线程II运行`TryReceiveMessage`，程序具体的运行如下：
1. 线程I在处理器本地进行了一些对`g_payload`的非原子性存储操作
2. 线程I启动了Release Fence ———— 线程I进行下次存储操作之后，所有它之前所进行的内存操作都会被其他线程看到
3. 线程I将值1存到`g_guard`中，注意此时由于Release Fence的存在，所有的内存操作已经同步到了其他处理器中(硬件层面的理解请参考[之前的博客](https://chocho-quan.github.io/2022/11/22/memory-barrier-ii/))
4. 一段时间之后线程II看到了`g_guard`的修改，并且进入了`if`分支
5. 线程II启动了Acquire Fence ———— 线程II保证了读到的payload的值至少和上一次读操作一样新(因为我们用Acquire Fence保证了先读`g_guard`再读`g_payload`)，至此同步语义完成

![synchronize](./images/sync0.png)

## Release Fence vs. Release Operations

我们在[DCLP的博客](https://chocho-quan.github.io/2022/11/01/dclp/)中，曾见到过类似`atmoic_var.load(sth, std::memory_order_release)`的Release Operation。值得注意的是，它和Release Fence并不等价！Release Fence可以完成Release Operations的功能，但反过来则并不是如此。参考以下的例子：

```cpp
// Release Operations
tmp = new Singleton;
m_instance.store(tmp, std::memory_order_release);

// Vs. Release Fence
tmp = new Singleton;
std::atomic_thread_fence(std::memory_order_release);
m_instance.store(tmp, std:memory_order_relaxed);
```

比起Fence，Release Operations实际上对内存操作重排(Memory Reordering)的限制要更少。Release Operation只保证了不会重排它之前的任何读写操作 & <font color=red><b>这次store操作</b></font>。而Release Fence则保证了不会重排此前任何的读写操作 & <font color=red><b>之后所有的写操作</b></font>。对Acquire Fence及Acquire Operations也是同理。

## 其他实现同步语义的方式

Acquire / Release Fence仅仅是实现同步语义的一种方式。实际上C++11之后，实现它的方式变得多种多样。Jeff整理并列出了以下的这些同步语义的方法(但并不完整~)
![synchronize](./images/sync1.png)
