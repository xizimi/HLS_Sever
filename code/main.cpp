#include <unistd.h>
#include "server/webserver.h"
#include "pool/redisconnpool.h"

int main() {
    // 初始化 Redis 连接池
    if (!RedisConnPool::Instance()->Init("127.0.0.1", 6379, 5)) {
        fprintf(stderr, "Failed to init Redis connection pool\n");
        return 1;
    }
    
    // 守护进程 后台运行 
    WebServer server(
        1316, 2, 60000,              // 端口 ET 模式 timeoutMs 
        3306, "zhaobowen", "huaji513612", "hls_sever", /* Mysql 配置 */
        12, 8, true, 1, 1024);             /* 连接池数量 线程池数量 日志开关 日志等级 日志异步队列容量 */

    server.Start();
}