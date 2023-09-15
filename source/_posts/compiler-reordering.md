---
title: 编译器指令重排(Compiler Instruction Reordering)的影响
date: 2022-12-11
author: Quan
tag: [Concurrency, C++, Memory Barrier]
category: C++ Low-level Concurrency
---

> This article is partly referred to & translated from [Jeff Preshing’s blog](https://preshing.com/20120625/memory-ordering-at-compile-time/). Still experiments are done by myself & some thoughts added.

曾经我写完一份C++代码，就会放心地把它交给编译器，因为剩下的它会帮我好好做完。但是实际上在你不知道的时候，你的编译器朋友(你的处理器朋友也会做一样的事)实际上有一套自己的玩法，它会根据一些设定好的规则在instruction level重排你的代码。这里的最基本的规则就是

> 指令的重排不能影响单线程程序的行为

这就是为什么我们大多数人可以放心的把自己的代码交给编译器。但当你离开单线程的世界，打开concurrency的大门，这就不是你再可以忽视的东西了。当然很多时候我们会用锁、atomic、信号量等等的设计来防止他们对多线程造成问题，但在无锁的代码片段里，或者甚至你想写无锁化编程时，这些问题都可能以你意想不到的形式出现。

这篇博客想讲明白以下三个问题，编译器的指令重排做了什么，对我们编程者而言有什么影响，需要注意什么。

## 什么是编译器指令重排(Compiler Instruction Reordering)

我们从底下这个例子开始看起

```cpp
int A, B;

void foo() {
    A = B + 1;
    B = 0;
}
```

在x86-64 gcc-12.2上编译结果如下

```x86asm
...
mov     eax, DWORD PTR B[rip]
add     eax, 1
mov     DWORD PTR A[rip], eax
mov     DWORD PTR B[rip], 0
...
```

汇编程序完美地复制了我们C++代码的结果，对B的memory操作在A之前。然而假设我们开启优化选项`-O2`，结果如下

```x86asm
...
mov     eax, DWORD PTR B[rip]
mov     DWORD PTR B[rip], 0
add     eax, 1
mov     DWORD PTR A[rip], eax
...
```

Boom!你的编译器朋友决定行使它作为编译器的权利，B的存储被重排到了A的前面！重新审视这段汇编代码，你会发现我们刚说的基本原则确实没有打破，如果这是个单线程的程序，你什么也不会发现，一切都是完美如初。

## 多线程的困扰 & 显式的编译器内存屏障(Compiler Memory Barrier)

但我们已经长大了，你连工作的同时都还要兼顾和朋友微信摸鱼聊天，程序怎么会单线程地运行呢？一个非常经典的publisher多线程问题如下：

```cpp
int Value;
int IsPublished = 0;

// This is in thread I
void sendValue(int x) {
    Value = x;
    IsPublished = 1;
}

// This is in thread II
int tryRecvValue() {
    if (IsPublished) return Value;
    return -1;  // or some other value to mean not yet received
}
```

假设不存在编译器重排，每当线程II检查`IsPublish`时，`Value`都一定是已经更新的值了，这种情况下我们很幸运地不会遇到任何的问题(在single-core机器上，如果在multi-core上，则要求机器上强内存模型)。然而如果发生了像之前所述的重排，那么当线程II执行到检查`IsPublish`时，有可能`Value`并未被更新！

最简单直接的解决方法就是编译器内存屏障(Compiler Barrier)

```cpp
#define COMPILER_BARRIER() asm volatile("" ::: "memory")

int Value;
int IsPublished = 0;

void sendValue(int x) {
    Value = x;
    COMPILER_BARRIER();          // prevent reordering of stores
    IsPublished = 1;
}

int tryRecvValue() {
    if (IsPublished) {
        COMPILER_BARRIER();      // prevent reordering of loads
        return Value;
    }
    return -1;  // or some other value to mean not yet received
}
```

关键的这行内嵌汇编代码`asm volatile("" ::: "memory")`实际上就是在告诉编译器，这里发生了内存的改动，不要进行编译器重排。在单核系统上，编译器屏障就能够解决上面的问题。但是在多核系统更为常见的今天，想要完全解决这个问题，我们还需要处理器维度的内存屏障(processor level memory barrier)。在这里我们暂时不讨论这个问题。

## 隐式的编译器内存屏障

本质上，大多数的函数调用都是自带编译器内存屏障的。想象作为一个编译器，你调用了一个外部函数，而你并不知道这个外部函数在做什么、有什么影响。因此编译器会把它当做进行了内存操作。我们来看下面的例子

```cpp
void doSomeStuff(Foo* foo) {
    foo->bar = 5;
    sendValue(123);       // prevents reordering of neighboring assignments
    foo->bar2 = foo->bar;
}
```

假设我们的`sendValue`是外部lib的函数，编译器怎么能知道它会不会改动 / 依赖`foo->bar`的值呢？所以为了保证基本原则(单线程程序不出问题)，编译器自然是不会改变这里的顺序的。对后面的`foo->bar2`的赋值也是一样的隐式地禁止了重排。同样的用gcc编译上面的代码，我们会看到下面的汇编

```x86asm
...
mov     rax, QWORD PTR [rbp-8]
mov     DWORD PTR [rax], 5 ;; foo->bar = 5
mov     edi, 123
call    sendValue(int) ;; sendValue(123)
mov     rax, QWORD PTR [rbp-8]
mov     edx, DWORD PTR [rax]
mov     rax, QWORD PTR [rbp-8]
mov     DWORD PTR [rax+4], edx ;; foo->bar2 = foo->bar
...
```

指令没有再被重排，而是完全的按照我们高级语言的顺序执行。

## 编译器产生的内存操作

编译器的权利不只是重排指令而已，它甚至可以新增一些内存操作。而这样的多线程问题在C++11以前是没有官方的解决方法的，下面是个简单的例子

```cpp
int A, B;

void foo() {
    if (A) B++;
}
```

在C++11标准之前我们编译之后可能会得到以下的代码
```cpp
void foo() {
    register int r = B;    // Promote B to a register before checking A.
    if (A) r++;
    B = r;          // Surprise! A new memory store where there previously was none.
}
```

此时哪怕A为False，我们依旧会改动B！同样地，单线程的情况下没有任何问题，再次符合了我们的基本原则。然而多线程的情况下，假设其他线程改动了B，不管A是不是False，我们都会直接改动B从而导致其他线程的改动完全失效！

当然会有人对gcc提出了[质疑](https://gcc.gnu.org/legacy-ml/gcc/2007-10/msg00266.html)，目前C++11标准已经禁止编译器进行这样的行为。现在如果我们再用gcc编译上面的代码会得到以下汇编
```x86asm
...
mov     eax, DWORD PTR A[rip]
test    eax, eax
je      .L3
mov     eax, DWORD PTR B[rip] ;; put B in some register
add     eax, 1 ;; add to the register
mov     DWORD PTR B[rip], eax ;; store it back to B
...
.L3:
nop
pop     rbp
ret
```

虽然编译器还是没有直接给B的地址进行++操作(注：在x86-64 gcc-12.2的编译环境下，如果你开启`-O2`优化，将会是直接给B的地址进行++操作)，但是此时的False分支里已然没有了对B的存储。
