#include "redisconnpool.h"
#include <iostream>

RedisConnPool* RedisConnPool::Instance() {
    static RedisConnPool instance;
    return &instance;
}

bool RedisConnPool::Init(const std::string& host, int port, int maxConn) {
    host_ = host;
    port_ = port;
    maxConn_ = maxConn;
    
    for (int i = 0; i < maxConn; ++i) {
        redisContext* conn = redisConnect(host.c_str(), port);
        if (conn && conn->err == 0) {
            connPool_.push_back(conn);
        } else {
            if (conn) redisFree(conn);
            return false;
        }
    }
    return true;
}

redisContext* RedisConnPool::GetConnection() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (connPool_.empty()) {
        redisContext* conn = redisConnect(host_.c_str(), port_);
        if (conn && conn->err == 0) return conn;
        if (conn) redisFree(conn);
        return nullptr;
    }
    redisContext* conn = connPool_.back();
    connPool_.pop_back();
    return conn;
}

void RedisConnPool::ReturnConnection(redisContext* conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(mtx_);
    if (connPool_.size() < maxConn_) {
        connPool_.push_back(conn);
    } else {
        redisFree(conn);
    }
}

RedisConnPool::~RedisConnPool() {
    for (auto conn : connPool_) {
        redisFree(conn);
    }
    connPool_.clear();
}