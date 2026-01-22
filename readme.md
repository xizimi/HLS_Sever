# C++ 轻量级高性能 HLS 点播服务器
## Introduction
用C++实现的高性能HLS点播服务器，

## Function
* 利用IO复用技术Epoll与线程池实现多线程的Reactor高并发模型；
* 利用正则与状态机解析请求客户端POST/GET报文；
* 接收数据使用LT模式每接收一部分数据就存入文件降低内存占用；
* 通过将视频文件分成多个chunk分片发送接收并根据complete请求将云端视频块合并并转码为HLS格式(.ts+.m3u8)实现断点续传；
* 多个分辨率版本m3u8文件可供客户端根据传输速度自由选择；
* 用mysql数据库存储请求视频id与实际存储目录，真实文件路径对客户端不可见，实现安全存储；
* 利用标准库容器封装char，实现自动增长的缓冲区；
* 基于小根堆实现的定时器，关闭超时的非活动连接；
* 利用单例模式与阻塞队列实现异步的日志系统，记录服务器运行状态；
* 利用RAII机制实现了数据库连接池，减少数据库连接建立与关闭的开销。


## Environment
* Ubuntu 18
* Modern C++
* MySql
* Vscode
* git

## Build & Usage
```
make
./bin/server

需要先配置好对应的数据库
bash
// 建立yourdb库
create database yourdb;

// 创建user表
USE yourdb;
CREATE TABLE user(
    id  varchar(32)  NULL,
    original_name varchar(255) NULL,
    hls_path text  NULL,
    status enum('uploading','processing','ready','failed') uploading,
    created_at timestamp current_timestamp()
)ENGINE=InnoDB;


## Test
```bash
POST、GET测试：
cd test_newlog
make min_test -o test
./test
make get_test -o get_test
./get_test





## Thanks 

[@JehanRio7](https://github.com/JehanRio/TinyWebServer)

