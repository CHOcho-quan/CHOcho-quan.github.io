---
title: Open File Description Lock简介
author: Quan
tag: [Linux System]
category: Operating System
date: 2023-07-19
---

在工作中遇到个非常非常有意思的项目，想着反正博客没什么人看，而且我司离倒闭估计是不远了，给大家简单介绍下背景。总而言之是，感知组突发奇想想用4K的图像来进行onboard推理（虽然我觉得你要跑个4k图像的模型，很难说performance能多好），而这一奇思妙想对onboard组来说是个比较大的考验：
* 如果真的使用中间件传输4K的图像掉帧率将会很高很高
* 因为图像一开始存在GPU上，那么从GPU拷贝到本地，再传输给感知的进程，最后感知再上传到GPU进行inference，多次host-device的传输将导致非常大的delay

在Profiling过后，我们发现最重要的bottleneck其实是host-device之间的image搬运过程，于是我们设计了以下这个onboard信息流来优化整个传输：

![](./images/inter-gpu.png)

整体上来说，就是我们通过进程间通信来告诉感知，你们需要的image被放在了GPU的什么位置，那么我们只需要在GPU上做一次device2device的拷贝，就可以开始进行推理。而这里的进程通信比普通的进程通信要多了一点东西，即：在感知进程使用GPU上的图像进行推理的时候，我们不能让sensor节点更新这里的图像。也就是说，我们需要一把进程间的锁，来保证这里的数据安全性。本篇博客关注于OFD Lock的设计、背景和优势，具体怎么使用并不会过多cover，详细的可以参考[man文档](https://manpages.ubuntu.com/manpages/focal/en/man2/flock.2.html)。

## 为什么是OFD Lock

背景部分我们讲到我们需要进行进程间通信，而我们为了performance考虑使用了共享内存。在读取共享内存时 / GPU端进行device2device拷贝时，按照刚才的讨论，我们需要一把锁来保证进程间的通信安全性。

在简单的调研之后，我们锁定到了两种锁，一是boost实现的inter-process mutex，第二就是今天的题目：OFD Lock。我并不可以把实验的数据列出来，但是经过详尽的experiments，基本的对比如下：

|             | OFD Lock | Boost Lock |
|-------------|----------|------------|
| Performance | < 1ms    | < 0.1ms    |
| Correctness | 100%     | 99.8%      |

这里可以观察到(不知道是我们实验的问题 or boost的实现仍旧有bug)，boost的进程间锁可能出现锁上之后却仍可以在另一个进程Acquire的情况。

除此之外，我们还发现了一个OFD Lock的巨大优势：在进程终止的时候，[kernel会自动关闭所有的文件](https://stackoverflow.com/questions/15246833/is-it-a-good-practice-to-close-file-descriptors-on-exit)。而后面我们会讲到OFD Lock是与一个文件相关联的，当所有file description被关闭时，OFD Lock会自动释放！而这代表了我们不需要做额外的exception handling来保证不出现dangling mutex的问题。到此，OFD Lock成了今天博客的主题。

## File Descriptor vs. File Description

要理解OFD Lock，就像这个名字所暗示的，你需要先简单地了解linux的文件系统。

我们最常见的打开文件的函数open()在Linux中的定义如下：
```c++
#include <fcntl.h>
int open(const char *pathname, int flags);
```

这里返回了一个整数值，它就是Open File Descriptor(也是我们常说的fd)。fd是在进程中，描述打开的文件的描述符。需要注意的是，一个文件可以对应多个fd，比如当你使用open()在同一个进程中打开两次文件时，你得到的fd也会是不一样的，但他们指向同一个文件。而在不同进程中，不同文件的fd可以是一样的，比如进程a中打开文件b的fd可以是1，进程b中打开文件a之后fd也可以同时为1。
```c++
// In same Process
auto a = open("lets_test");
auto b = open("lets_test");
// a is not equal to b!
assert(a != b);
```

而Open File Description和fd最大的不同是，它是在os中的概念而非在进程中的概念。它是一个在整个os中被打开的文件的描述符，但同时需要注意，它并不代表文件本身！简单来说还是刚才的例子，假设你使用open()在同一个进程中打开两次文件，你得到的open file description和fd都会不一样，虽然它们都指向同一个文件。如果你仔细想这件事会发现它其实很合理，因为file description指代的是一个文件被打开的状态，比如现在的offset在哪儿，是READ模式还是WRITE模式，etc.，你肯定不希望打开两次文件之后，这两个文件的offset都一样，且连读写模式都一样吧？

当然如果你想要两个fd指向同样的file description，dup或者fork都可以满足你的需求。
```c++
// In same Process
// a and b have different file description and file descriptor!
auto a = open("test2");
auto b = open("test2");
// a and c shares file description but different file descriptor
auto c = dup(a);
assert(a != c);
```

底下的这张图很好的描述了fd / file description和vNode(代表了真正意义上的文件)之间的联系(sources from [here](https://www.usna.edu/Users/cs/wcbrown/courses/IC221/classes/L09/Class.html))。
![](./images/sys-ofd.png)

## OFD Lock

### 基本用法

用法上，man page已经足够详尽，我只想cover一个最简单的例子以保证一下完整性。

```cpp
struct flock lck = {
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 1,
};
// Open some file
auto fd = open("/tmp/ofd_lock_file", O_RDWR | O_CREAT, 0666);
lck.l_type = F_WRLCK; // Use F_RDLCK here for Read only clock
fcntl (fd, F_OFD_SETLK, &lck);
// Do your things
// You have to reset the clock as UNLCK, or pack it RAII
lck.l_type = F_UNLCK;
fcntl (fd, F_OFD_SETLK, &lck);
```

### 更多细节

OFD Lock，锁如其名，作用在Open File Description上。需要注意的是，它锁下的是一个文件，指向同一个文件的不同的Open File Description会冲突，而相同的Open File Description并不会conflict。上面的例子里，假设我们使用dup来克隆一个file(这意味着这两个fd有相同的open file description)。以下这个例子里的th1和th2并不会成功地锁住，因为它们拥有相同的Open File Description。
```cpp
// Open some file, fd & fd2 shares open file description
auto fd = open("/tmp/ofd_lock_file", O_RDWR | O_CREAT, 0666);
auto fd2 = dup(fd);

auto th1 = std::thread([fd]() {
    lck.l_type = F_WRLCK; // Use F_RDLCK here for Read only clock
    fcntl (fd, F_OFD_SETLKW, &lck); // blocking
    // Do your things
    // You have to reset the clock as UNLCK, or pack it RAII
    lck.l_type = F_UNLCK;
    fcntl (fd, F_OFD_SETLK, &lck);
});

auto th2 = std::thread([fd2]() {
    lck.l_type = F_WRLCK; // Use F_RDLCK here for Read only clock
    fcntl (fd2, F_OFD_SETLKW, &lck); // blocking
    // Do your things
    // You have to reset the clock as UNLCK, or pack it RAII
    lck.l_type = F_UNLCK;
    fcntl (fd2, F_OFD_SETLK, &lck);
});
```

具体的底层机制(OFD Lock的实现)我并不清楚，但合理地推测是在vNode的level完成锁的操作，因为如果要进行文件IO的操作，而不是在kernel space完成，OFD Lock的performance应该不会那么好。

### O_CLOEXEC的作用

我们提到过OFD Lock的一大优势就是Linux在关闭进程时会关闭对应的fd，那么相应的Open File Description会在所有对应它的fd关闭后自动关闭，而我们的OFD Lock也就自然释放了。但一大风险是，当你fork() / exec()创建子进程时，这些fd是自动被继承的。而当你不知道这件事存在时，可能会造成你以为OFD Lock释放了，但是其实还没有。加上这个flag就能够控制子进程不再继承这些锁，防止出现我们所说的问题。
