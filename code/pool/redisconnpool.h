#ifndef REDIS_CONN_POOL_H
#define REDIS_CONN_POOL_H

#include <string>
#include <vector>
#include <mutex>
#include <hiredis/hiredis.h>

class RedisConnPool {
public:
    static RedisConnPool* Instance();
    
    bool Init(const std::string& host, int port, int maxConn);
    redisContext* GetConnection();
    void ReturnConnection(redisContext* conn);
    
private:
    RedisConnPool() = default;
    ~RedisConnPool();
    
    std::vector<redisContext*> connPool_;
    std::mutex mtx_;
    int maxConn_;
    std::string host_;
    int port_;
};

#endif // REDIS_CONN_POOL_H