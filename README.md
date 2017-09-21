# KMSAN (KernelMemorySanitier)

`KMSAN` is a detector of uninitialized memory use for the Linux kernel. It is
currently in development.

Contact: ramosian-glider@

## Code

*   The kernel branch with KMSAN patches is available at https://github.com/google/kmsan
*   Patches for LLVM r304977: [LLVM patch](https://github.com/google/kmsan/blob/master/kmsan-llvm.patch),
    [Clang patch](https://github.com/google/kmsan/blob/master/kmsan-clang.patch)
*   Clang wrapper: https://github.com/google/kmsan/blob/master/clang_wrapper.py

## How to build

In order to build a kernel with KMSAN you'll need a custom Clang built from a patched tree on LLVM r298239.

```
export WORLD=`pwd`
```

### Build Clang
```
R=304977
svn co -r $R http://llvm.org/svn/llvm-project/llvm/trunk llvm
cd llvm
(cd tools && svn co -r $R http://llvm.org/svn/llvm-project/cfe/trunk clang)
(cd projects && svn co -r $R http://llvm.org/svn/llvm-project/compiler-rt/trunk compiler-rt)
wget https://raw.githubusercontent.com/google/kmsan/master/kmsan-llvm.patch
patch -p0 -i kmsan-llvm.patch
wget https://raw.githubusercontent.com/google/kmsan/master/kmsan-clang.patch
patch -p0 -i kmsan-clang.patch
mkdir llvm_cmake_build && cd llvm_cmake_build
cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON ../
make -j64 clang
export KMSAN_CLANG_PATH=`pwd`/bin/clang
```

### Configure and build the kernel
```
cd $WORLD
git clone https://github.com/google/kmsan.git kmsan
cd kmsan
# Now configure the kernel. You basically need to enable CONFIG_KMSAN and CONFIG_KCOV,
# plus maybe some 9P options to interact with QEMU.
cp .config.example .config
# Note that clang_wrapper.py expects $KMSAN_CLANG_PATH to point to a Clang binary!
make CC=`pwd`/clang_wrapper.py -j64 -k 2>&1 | tee build.log
```

### Run the kernel
You can refer to https://github.com/ramosian-glider/clang-kernel-build for the instructions
on running the freshly built kernel in a QEMU VM.
Also consider running a KMSAN-instrumented kernel under [syzkaller](https://github.com/google/syzkaller).

## Trophies

*   [`tmp.b_page` uninitialized in
    `generic_block_bmap()`](https://lkml.org/lkml/2016/12/22/158)
    *   Writeup:
        https://github.com/google/kmsan/blob/master/kmsan-first-bug-writeup.txt
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/2a527d6858c246db8afc3d576dbcbff0902f933b)
*   [`strlen()` called on non-terminated string in `bind()` for
    `AF_PACKET`](https://lkml.org/lkml/2017/2/28/270)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/540e2894f7905538740aaf122bd8e0548e1c34a4)
*   [too short socket address passed to
    `selinux_socket_bind()`](https://lkml.org/lkml/2017/3/3/524)
    *   Status: reported upstream
*   [uninitialized `msg.msg_flags` in `recvfrom`
    syscall](https://lkml.org/lkml/2017/3/7/361)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/9f138fa609c47403374a862a08a41394be53d461)
*   incorrect input length validation in `nl_fib_input()`
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/c64c0b3cac4c5b8cb093727d2c19743ea3965c0b)
        by Eric Dumazet
*   [uninitialized `sockc.tsflags` in
    `udpv6_sendmsg()`](https://lkml.org/lkml/2017/3/21/505)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/d515684d78148884d5fc425ba904c50f03844020)
*   [incorrect input length validation in
    `packet_getsockopt()`](https://lkml.org/lkml/2017/4/25/628)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/fd2c83b35752f0a8236b976978ad4658df14a59f)
*   [incorrect input length validation in `raw_send_hdrinc()`
    and `rawv6_send_hdrinc()`](https://lkml.org/lkml/2017/5/3/351)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/86f4c90a1c5c1493f07f2d12c1079f5bf01936f2)
*   [missing check of `nlmsg_parse()` return value in
    `rtnl_fdb_dump()`](https://lkml.org/lkml/2017/5/23/346)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/0ff50e83b5122e836ca492fefb11656b225ac29c)
*   [Linux kernel 2.6.0 to 4.12-rc4 infoleak due to a data race in ALSA timer](http://openwall.com/lists/oss-security/2017/06/12/2) ([CVE-2017-1000380](http://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2017-1000380))
    *   Status: fixed upstream ([1](https://github.com/torvalds/linux/commit/ba3021b2c79b2fa9114f92790a99deb27a65b728), [2](https://github.com/torvalds/linux/commit/d11662f4f798b50d8c8743f433842c3e40fe3378))
*   [`strlen()` incorrectly called on user-supplied memory in `dev_set_alias()`](https://lkml.org/lkml/2017/5/31/394)
    *   Status: [fixed
        upstream](https://github.com/torvalds/linux/commit/c28294b941232931fbd714099798eb7aa7e865d7)

### Confirmed bug reports by others:
*   [`deprecated_sysctl_warning()` reads uninit memory](https://lkml.org/lkml/2017/5/24/498)
*   [`struct sockaddr` length not checked in `llcp_sock_connect()`](https://github.com/torvalds/linux/commit/608c4adfcabab220142ee335a2a003ccd1c0b25b)
