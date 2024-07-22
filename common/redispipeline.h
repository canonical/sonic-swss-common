#pragma once

#include <string>
#include <functional>
#include <queue>
#include <chrono>
#include <iostream>
#include "logger.h"
#include "redisreply.h"
#include "rediscommand.h"
#include "dbconnector.h"

namespace swss {

class RedisPipeline {
public:
    const size_t COMMAND_MAX;
    static constexpr int NEWCONNECTOR_TIMEOUT = 0;

    RedisPipeline(const DBConnector *db, size_t sz = 128)
        : COMMAND_MAX(sz)
        , m_remaining(0)
        , m_shaPub("")
    {
        m_db = db->newConnector(NEWCONNECTOR_TIMEOUT);
        lastHeartBeat = std::chrono::steady_clock::now();
    }

    ~RedisPipeline();
    
    redisReply *push(const RedisCommand& command, int expectedType);

    redisReply *push(const RedisCommand& command);

    std::string loadRedisScript(const std::string& script);

    void flush();

    size_t size();

    int getDbId();

    std::string getDbName();

    DBConnector *getDBConnector();

    void addChannel(std::string channel);

    int getIdleTime(std::chrono::time_point<std::chrono::steady_clock> tcurrent=std::chrono::steady_clock::now());

private:
    DBConnector *m_db;
    std::queue<int> m_expectedTypes;
    size_t m_remaining;
    std::string m_luaPub;
    std::string m_shaPub;
    std::chrono::time_point<std::chrono::steady_clock> lastHeartBeat;

    // The caller is responsible to release the reply object
    redisReply *pop();

    void mayflush();
    void publish();
};

}
