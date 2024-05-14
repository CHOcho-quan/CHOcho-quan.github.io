---
title: Log To Terminal的潜在风险
date: 2024-05-14 16:18:23
author: Quan
tag: [Glog, C++, Sleep, Logging, Operating System]
category: Logging
---

工作中在做performance breakdown的时候，发现了一个非常有趣的问题。我们从以下这段代码开始
```cpp
// Setting env variable to output to terminal
setenv(GLOG_logtostderr, "1");

while (true) {
    // Do something else, application codes
    // Note this function cost almost no time
    DoSomething();
    LOG(INFO) << "Yeah";
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
```

最初我们发现，这段代码的单个loop循环时间可以达到20ms。当时我不以为意，觉得系统scheduling导致的sleep function误差就可能有这么大的，毕竟当时测试时的CPU Util也达到了90%。到后来，在某次我做压力测试将系统的CPU Util调高到了100%之后，神奇的事情发生了：单个loop循环可以整整花掉30s。

这个简直不可思议，经过简单的breakdown，我可以确定`DoSomething()`基本不花时间。剩下的codes也就只剩Glog和sleep_for了，我们首先来看sleep_for。

## 走进sleep_for

C++ Reference中的[sleep_for](https://en.cppreference.com/w/cpp/thread/sleep_for)就明确地写了，由于系统调度的问题，函数可能会睡更长的时间。但是系统会保证`sleep_for`只会多睡不会少睡。那接下来的问题就是，首先sleep_for到底会多睡多少呢？

下面的表格展示了在不同的CPU Util之下，使用`sleep_for(2ms)`时具体的Max / Avg / Min睡眠时间，实验时间为30分钟。

| Background CPU Util | Max / ms | Avg / ms | Min / ms |
| ----------- | ----------- | ----------- | ----------- | 
| 50 | 4.64 | 2.06 | 2.01 |
| 70 | 6.64 | 2.06 | 2.01 |
| 90 | 9.91 | 2.06 | 2.00 |
| 100 | 18.92 | 2.06 | 2.00 |

很符合我最初的预期，在没有合适调度的情况下，sleep将有毫秒级别的误差。接下来我想验证的是，如果我有了合适的调度，是不是这种误差就能够减少甚至消失了呢？所以我做了第二个实验，在这个实验里，我给我的sleep进程调度策略设置为[SCHED_FIFO](https://man7.org/linux/man-pages/man7/sched.7.html)，并且将优先级调到了最高。下面的表格展示了在不同的CPU Util之下，如果有合适调度之后，使用`sleep_for(2ms)`时具体的Max / Avg / Min睡眠时间，实验时间为30分钟。

| Background CPU Util | Max / ms | Avg / ms | Min / ms |
| ----------- | ----------- | ----------- | ----------- | 
| 50 | 2.98 | 2.01 | 2.01 |
| 70 | 3.24 | 2.01 | 2.01 |
| 90 | 3.27 | 2.01 | 2.00 |
| 100 | 4.51 | 2.01 | 2.00 |

可以看到，系统的睡眠时间明显贴近了2ms，即便在100% CPU Util的情况下，误差最大也只有2ms左右。

这是很酷的结果，但别忘了我们最初的问题：30秒的delay来源于哪儿呢？通过这里的实验，我们已经知道，这30秒绝对不可能是系统睡眠所带来的，那么剩下的就是glog了。

## Glog做了什么吗

我赶紧跑了跑strace，结果告诉我：没错就是Glog出了问题！一行write的System call可以花到12秒！
![](./images/strace_result.png)

接下来我先简单读了读glog的代码，底下是每行LOG会调用的核心函数
```cpp
inline void LogDestination::LogToAllLogfiles(LogSeverity severity,
                                             time_t timestamp,
                                             const char* message,
                                             size_t len) {

  if ( FLAGS_logtostderr ) {           // global flag: never log to file
    ColoredWriteToStderr(severity, message, len);
  } else {
    for (int i = severity; i >= 0; --i)
      // This will call your logger Write
      LogDestination::MaybeLogToLogfile(i, timestamp, message, len);
  }
}
```

因为启用了`GLOG_logtostderr`的flag，所以我们每次都会调用`ColoredWriteToStderr`来写入terminal。在这个函数中，每次glog会调用`fwrite`来写入文件。我在怀疑的是可不可能是因为fwrite实际上flush到了文件，所以导致了任何disk IO而产生了那么长的delay？

关于`fwrite`是会cache还是直接flush到terminal，网友们基本上有两种讲法
* 如果有`\n`，对terminal来说会flush
* 根本不会flush

我简单地做了一个实验，我使用cout来print多行结果，观察cout需要的时间。表格如下
| Flush | Max / ms | Avg / ms | Min / ms |
| ----------- | ----------- | ----------- | ----------- | 
| With ‘\n’ to stderr | 10257 | 242 | 0.96 |
| Without ‘\n’ to stderr | 10433 | 163 | 0.66 |
| With ‘\n’ to stdout | 10165 | 247 | 0.82 |
| Without ‘\n’ to stdout | 10562 | 232 | 0.58 |

基本的结论就是，`\n`至少在我的linux环境下并不会影响flush。

而且说到底，即便是disk IO也不太可能花掉整整30s的时间。假设一张非常菜的SATA SSD，带宽只有1G/s。那么30s再糟也要写个10G的文件吧，然而我们这里只是一行简单的log，甚至是写到terminal（所以本质上甚至不需要disk IO），为什么会需要这么长的时间呢？

## Terminal做了什么吗

接下来我看向了Terminal，它的渲染过程(rendering)是不是有可能会花掉这么长时间呢？如果不print到terminal里，是不是就不会有这个问题了？

我一开始十分怀疑自己的怀疑，因为如果terminal的渲染过程会block我的logging，那终端开发者也太烂了。可是接下来的实验让我目瞪口呆：如果我只让glog写到文件里，不写到terminal就不会再有这个问题。而且如果我把terminal的output给redirect到文件中，问题也消失了！

| IO Method | Max / ms | Avg / ms | Min / ms |
| ----------- | ----------- | ----------- | ----------- | 
| Write to terminal & redirect to file | 7.62 | 0.05 | 0.004 |
| Write to terminal | 12480 | 0.1 | 0.003 |
| Write to file directly | 0.05 | 0.002 | 0.001 |

到这儿，已经基本说明了是terminal的渲染影响了Glog写terminal的速度，而且最差情况一行log可能会花整整12秒。接下来我非常好奇的是，为什么会有这种情况呢？凭什么terminal的渲染能够block我的logging output呢？

这篇[blog](https://www.linusakesson.net/programming/tty/index.php)部分解决了我的困惑。假设terminal只有一定的kernel buffer，当这个缓存已经满了的时候，而新的logging output又到了，terminal该怎么办呢？

很明显，作为一个terminal，你首先不能做的事情是扔掉一部分的log。否则你对不起你的职责，你是一个terminal。而这篇文章给的答案就是：blocking IO。那么到这儿，logging的问题就可以盖棺定论了，当terminal的buffer满了的时候，渲染过程block了我们的logging output。而这个渲染的过程完全可能花上以秒为单位的时间（当然这个可能根据不同terminal的实现有所不同）。

当然我的新问题是：尊敬的terminal开发者们，既然如此为什么不先把terminal的output写到一个文件里，然后再慢慢渲染呢？

我只有一个猜测的答案：这样terminal的实时性就无法保证了。

无论如何最后的结论也相当简单：在有高性能需求的环境下，不要往terminal写Log！！！