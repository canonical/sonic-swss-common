#include "redispipeline.h"

namespace swss {

RedisPipeline::~RedisPipeline()
{
    flush();
    delete m_db;
}

redisReply *RedisPipeline::push(const RedisCommand& command, int expectedType)
{
    switch (expectedType)
    {
        case REDIS_REPLY_NIL:
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_INTEGER:
        {
            int rc = redisAppendFormattedCommand(m_db->getContext(), command.c_str(), command.length());
            if (rc != REDIS_OK)
            {
                // The only reason of error is REDIS_ERR_OOM (Out of memory)
                // ref: https://github.com/redis/hiredis/blob/master/hiredis.c
                throw std::bad_alloc();
            }
            m_expectedTypes.push(expectedType);
            m_remaining++;
            mayflush();
            return NULL;
        }
        default:
        {
            flush();
            RedisReply r(m_db, command, expectedType);
            return r.release();
        }
    }
}

redisReply *RedisPipeline::push(const RedisCommand& command)
{
    flush();
    RedisReply r(m_db, command);
    return r.release();
}

std::string RedisPipeline::loadRedisScript(const std::string& script)
{
    RedisCommand loadcmd;
    loadcmd.format("SCRIPT LOAD %s", script.c_str());
    RedisReply r = push(loadcmd, REDIS_REPLY_STRING);

    std::string sha = r.getReply<std::string>();
    return sha;
}

void RedisPipeline::flush() {

    lastHeartBeat = std::chrono::steady_clock::now();

    if (m_remaining == 0) {
        return;
    }

    while(m_remaining)
    {
        // Construct an object to use its dtor, so that resource is released
        RedisReply r(pop());

    }
    publish();

}

int RedisPipeline::getIdleTime(std::chrono::time_point<std::chrono::steady_clock> tcurrent)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(tcurrent - lastHeartBeat).count());
}

size_t RedisPipeline::size()
{
    return m_remaining;
}

int RedisPipeline::getDbId()
{
    return m_db->getDbId();
}

std::string RedisPipeline::getDbName()
{
    return m_db->getDbName();
}

DBConnector *RedisPipeline::getDBConnector()
{
    return m_db;
}

void RedisPipeline::addChannel(std::string channel) {
    m_luaPub +=
    "redis.call('PUBLISH', '" + channel + "', 'G');";

    m_shaPub = loadRedisScript(m_luaPub);
}

redisReply *RedisPipeline::pop()
{
    if (m_remaining == 0) return NULL;

    redisReply *reply;
    int rc = redisGetReply(m_db->getContext(), (void**)&reply);
    if (rc != REDIS_OK)
    {
        throw RedisError("Failed to redisGetReply in RedisPipeline::pop", m_db->getContext());
    }
    RedisReply r(reply);
    m_remaining--;

    int expectedType = m_expectedTypes.front();
    m_expectedTypes.pop();
    r.checkReplyType(expectedType);
    if (expectedType == REDIS_REPLY_STATUS)
    {
        r.checkStatusOK();
    }
    return r.release();
}

void RedisPipeline::mayflush()
{
    if (m_remaining >= COMMAND_MAX)
        flush();
}

void RedisPipeline::publish() {
    if (m_shaPub == "") {
        return;
    }
    RedisCommand cmd;
    cmd.format(
        "EVALSHA %s 0",
        m_shaPub.c_str());
    RedisReply r(m_db, cmd);
}

} // namespace swss