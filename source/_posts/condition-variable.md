---
title: 条件变量(Condition Variable) Notify的底层机制
date: 2025-02-27
author: Quan
tag: [Concurrency, C++, Condition Variable]
category: C++ Low-level Concurrency
---

工作中又一次遇到了一个非常棘手但是有意思的问题，我们来看以下的代码

```cpp
// Some simple cv implementation
class MyClass { 
    std::mutex mtx;
    std::condition_variable cv;

    // Wait for notification
    void Wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock);
    }

    // Notify one waiting thread
    void Notify() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.notify_one();
    }
}
```

对熟悉condition variable(以下简称cv)的朋友来说，MyClass的实现是再简单不过的（实际情况下当然Notify中间会改动一些internal state，但总体实现就是这么简单）。那对于这么简单的一个cv实现，如果我给定如下条件：
1. `Wait()`会被多个线程所调用，`Notify()`只会被某一个线程调用
2. 整体CPU使用率偏高

请问当`Notify()`被调用时，`Wait()`可能会被卡住吗？答案当然是可能的，因为CPU的resource是有限的，而背景CPU使用率偏高，所以`Wait()`会自然可能因为拿不到资源而卡住。

那接下来的问题是：`Notify()`会卡住吗？

我原本会理所当然地认为：当然不会！不就是发个signal吗，`Wait()`此时是yield CPU resources的，不会有任何系统操作。哪怕`Notify()`本身需要某些os level的锁，只要不被并行的调用不也没问题吗？而我们给定的情况就是：`Notify()`只会被一个线程所调用，所以也不可能会被os mutex所卡住。

而答案是惊人的：在某些特定条件下，`Notify()`会卡住，而且卡住的时间竟然会和某些`Wait()`卡住的时间一致。接下来我们通过阅读Glibc的NPTL源码来一探究竟。

当然TL;DR: 在使用cv时，如果有多个thread在调用`wait()`，那么`notify_one()`会因为某些`wait()`无法被schedule上而卡住。卡住的时间为`wait()`的schedule delay。

## Glibc中的condition variable实现

首先，Glibc的NPTL实现非常复杂，这篇文章不想深入讨论诸如cv的MO实现、具体的wait/cancel，只想通过“为什么Notify()会卡住”这一个工作中实际遇到的问题作为契机，来理解这个问题本身。由此，整段源码分析不会纠结于细节，我们只关注一件事：何种情况下notify_one()会卡住。

以下几章中阅读的Glibc NPTL源码版本为[glibc-2.35](https://github.com/bminor/glibc/releases/tag/glibc-2.35)。

### 一读pthread_cond_signal()

在Linux下，`notify_one()`的实现本质是`pthread_cond_signal()`，为了探索为什么`notify_one()`会卡住，我们需要了解`pthread_cond_signal()`的实现。

Again，文章只关注为什么`notify_one()`会卡住，所以不会深入讨论`pthread_cond_signal()`的实现细节。

因此，我们只需要关住`pthread_cond_signal()`中哪里可能会被卡住。而阅读源码会发现代码中共有两处调用了`futex_wait_simple()`。阅读`futex_wait_simple()`的[官方文档](https://man7.org/linux/man-pages/man2/futex.2.html)可以发现，这个操作不是自旋的，而是会sleep。

第一处的`futex_wait_simple()`来自`__condvar_acquire_lock()`，这个很好理解，因为本质上就是获取一个os level的锁，便于对一个cv的内部结构进行操作。而第二个`futex_wait_simple()`则更加耐人寻味，它隐藏在这个名叫`__condvar_quiesce_and_switch_g1()`的函数中。

整个`pthread_cond_signal()`的实现非常简洁，我们会在之后的section中详细讨论。目前需要注意的有两点
1. 如果`cond->__data.__g_size[g1] == 0`，则`__condvar_quiesce_and_switch_g1()`会被调用，而该函数中会调用`futex_wait_simple()`
2. 当且仅当`cond->__data.__g_size[g1] != 0`，或者`__condvar_quiesce_and_switch_g1()`被调用后返回true，则`futex_wake()`会被调用。

```cpp
int
___pthread_cond_signal (pthread_cond_t *cond) {
    // Other codes omitted
    // Here if cond->__data.__g_size[g1] == 0, we'll call __condvar_quiesce_and_switch_g1()
    // which is possible to sleep
    if ((cond->__data.__g_size[g1] != 0)
        || __condvar_quiesce_and_switch_g1 (cond, wseq, &g1, private))
    {
        atomic_fetch_add_relaxed (cond->__data.__g_signals + g1, 2);
        cond->__data.__g_size[g1]--;
        do_futex_wake = true;
    }

    // If we entered the if block, we will do futex_wake(), which is actually notifying the waiters
    if (do_futex_wake)
        futex_wake (cond->__data.__g_signals + g1, 1, private);
}
```

那接下来我们就需要看看，第一：在这个cv的结构里，`__g_size`是什么？第二：`__condvar_quiesce_and_switch_g1()`为什么在此时调用，它要做什么？

### cv的结构

cv的结构相当简单，注释在`pthread_cond_wait()`的实现中十分清晰。需要特别关注的只有一点，我们会发现其中重要的数据结构如:`__g_size`，`__g_signals`，`__g_refs`都是一个大小为2数组。这里的2分别代表是什么呢？

再去研读`pthread_cond_wait()`的实现就会发现，整个cv的实现分成了两个组g1和g2，所有的`pthread_cond_wait()`都会先被分到g2，而`pthread_cond_signal()`永远只会给g1的waiters发送信号唤醒。而当g1中没有waiters的时候，`pthread_cond_signal()`会调用`__condvar_quiesce_and_switch_g1()`，将g1和g2互换。

![cv](images/cv.drawio.png)

这样的实现有一个最大的好处：简单。当`pthread_cond_wait()`被调用时，它只需要将waiter扔到g2，然后等待notify对它进行操作。而当`pthread_cond_signal()`被调用时，它只需要处理g1，并行的数据结构不至于很多，实现起来方便得多。

可是Concurrency问题会在g1为空，但是g2不为空的时候到来：我们需要安全的处理g2和g1的切换。自然我们知道要加锁，可是下一个问题是，假设切换的时候g1中存在一些waiters，它们虽然已经被signal了，但是因为各种原因没有被唤醒，我们要怎么办？

直接冒在脑子里的最简单的做法是：强行直接交换。反正G1中的waiter已经被signal过了，能不能被唤醒取决于操作系统，我不需要再担心。

要回答这个问题，我们需要来仔细看看cond_var中的结构
```cpp
struct __pthread_cond_s
{
  // Other codes omitted
  unsigned int __g_refs[2] __LOCK_ALIGNMENT; // size of waiters WIP, will decrease once a thread is waked up
  unsigned int __g_size[2]; // size of current waiters in each group, will decrease once signal is sent
  unsigned int __g_signals[2]; // signals for each group
};
```

在这个实现中，`__g_signals`存储的是一个真正的futex word，而与之共用的是`__g_refs`中存储了所有目前还在引用这个futex word的线程。如果强行交换，会导致可能存在大量的线程持有这一futex word，唤醒过后都会大量改动 / 使用其中的数据，最终导致可能的的ABA问题，足以想象其中实现的困难。

而glic最后的实现方式就是，如果g1存在一些waiters并没有被唤醒，则需要notify等候直到它被唤醒。接下来我们重新进入`pthread_cond_notify()`，来理解其中具体的实现。

### 重读pthread_cond_notify()

这一次我们从第一个condition开始，然后深入`__condvar_quiesce_and_switch_g1()`的实现，看看我们的理解是否正确。最重要的`if`条件如下

```cpp
if ((cond->__data.__g_size[g1] != 0)
    || __condvar_quiesce_and_switch_g1 (cond, wseq, &g1, private))
```

第一个condition `__g_size[g1] != 0`如我们上面所想，即g1内已经没有了正在等待的waiter。因为如果g1存在waiters，那么notify要做的事情非常简单，直接send信号唤醒即可。而如果g1里已经没有了任何waiters，接下来要做的就是交换g1 / g2，也就是`__condvar_quiesce_and_switch_g1()`所做的事情。

我们直接来读其中调用`futex_wait_simple()`的段落，来看具体到底是什么条件会使得notify的thread被Block住。
```cpp
// Fetch __g_refs[g1]
unsigned r = atomic_fetch_or_release (cond->__data.__g_refs + g1, 0);
while ((r >> 1) > 0) {
    // Spin for some time to see if __g_refs get changed
    for (unsigned int spin = maxspin; ((r >> 1) > 0) && (spin > 0); spin--) {
	  r = atomic_load_relaxed (cond->__data.__g_refs + g1);
	}
    
    if ((r >> 1) > 0) {
	  r = atomic_fetch_or_relaxed (cond->__data.__g_refs + g1, 1) | 1;

	  if ((r >> 1) > 0)
	    futex_wait_simple (cond->__data.__g_refs + g1, r, private);
	  /* Reload here so we eventually see the most recent value even if we
	     do not spin.   */
	  r = atomic_load_relaxed (cond->__data.__g_refs + g1);
	}
}
```

本质上就是判断`__g_refs[g1]`是否大于0 ———— 即有没有一个thread是被signal过，却没有唤醒的（之所以`r >> 1`是因为`__g_refs`的第一位是另有作用的，从第二位开始表达是否有thread仍在引用futex）。

合二为一，总结而言就是在以下条件满足时，`notify_one()`有可能会跑的很慢
* 有一个waiting thread已经被signal过了，但由于各种原因(如CPU资源不够)没有被唤醒
* 有一个waiting thread是在上一个signal后开始wait(g2)

## 实验证明

由此我们就可以用实验证明我们的理论，这个程序也极其容易写(见附录)，只要在强行使用`taskset -c 11 chrt -f 25 stress-ng -c 11 -l 100`卡死一个核，接着把waiting thread绑定在这个核上，这可以手动构造出signal但是没有醒的情况。在第二个notify调用之前，再开一个新的waiting thread(使其加入g2)，最终就可以得到实验结果如下
![](images/cv_expr.png)

## Takeaways

到这里，我们就可以总结得出一些使用cv的principle
* waiting thread需要有比较高的priority，使得它们可以被及时schedule上
* 一对一的cv使用wait - notify会是最好的方式，如果有多个thread，可以尝试使用多个cv

## Appendix

```cpp
class Test {
 public:
   std::condition_variable cv;
   std::mutex mtx;
   bool flag = false;
   std::thread thread;
   std::thread thread2;

   Test() {
     thread = std::thread([this]() {
       auto start = std::chrono::system_clock::now();
       while (std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - start).count() < 60) {
         std::unique_lock<std::mutex> lk(mtx);
         cv.wait(lk, [this]() { return flag; });
         break;
       }
       std::cout << "thread1 done" << std::endl;
     });

     // Bind it to CPU 11, and stress CPU 11 with 100% Util and FIFO policy with HIGH priority
     pthread_setname_np(thread.native_handle(), "test_thread");
     cpu_set_t cpu_set;
     CPU_ZERO(&cpu_set);
     CPU_SET(11, &cpu_set);
     pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpu_set);
   }

   void CreateThread2() {
     thread2 = std::thread([this]() {
       while (true) {
         std::unique_lock<std::mutex> lk(mtx);
         cv.wait(lk, [this]() { return flag; });
         flag = false;
         break;
       }
       std::cout << "thread2 done" << std::endl;
     });
   }

   ~Test() {
     thread.join();
     if (thread2.joinable()) {
       thread2.join();
     }
   }

   void Notify() {
     mtx.lock();
     flag = true;
     mtx.unlock();

     cv.notify_one();
   }
};

int main(int argc, char* argv[]) {
 // Thread 1 will go to cv g1
 Test test;
 std::this_thread::sleep_for(std::chrono::milliseconds(1000));

 auto test_start = std::chrono::system_clock::now();
 // We try to make thread1 not able to be waked up
 // Notify will decrease __g_size[g1] while __g_refs[g1] is still 1
 test.Notify();
 auto test_end = std::chrono::system_clock::now();
 auto cnt = std::chrono::duration_cast<std::chrono::milliseconds>(test_end - test_start).count();
 std::cout << "test time: " << cnt << "ms" << std::endl;

 std::this_thread::sleep_for(std::chrono::milliseconds(100));
 test.flag = false;
 // New waiters would go to g2
 test.CreateThread2();
 std::this_thread::sleep_for(std::chrono::milliseconds(100));
 test_start = std::chrono::system_clock::now();
 // For this notify, thread2 is waiting in g2, cv will try to switch group
 // However, thread1 is still in g1 (not woken up), thus this notify would run for a long time
 test.Notify();
 test_end = std::chrono::system_clock::now();
 cnt = std::chrono::duration_cast<std::chrono::milliseconds>(test_end - test_start).count();
 std::cout << "test time: " << cnt << "ms" << std::endl;
 return 0;
}
```
