#include <string.h>
#include <stdint.h>
#include <vector>
#include <unistd.h>
#include <errno.h>
#include <system_error>
#include <fstream>
#include "json.hpp"
#include "logger.h"

#include "common/dbconnector.h"
#include "common/redisreply.h"

using json = nlohmann::json;
using namespace std;

namespace swss {

void SonicDBConfig::parseDatabaseConfig(const string &file,
                    std::unordered_map<std::string, RedisInstInfo> &inst_entry,
                    std::unordered_map<std::string, SonicDBInfo> &db_entry,
                    std::unordered_map<int, std::string> &separator_entry)
{
    ifstream i(file);
    if (i.good())
    {
        try
        {
            json j;
            i >> j;
            for (auto it = j["INSTANCES"].begin(); it!= j["INSTANCES"].end(); it++)
            {
               string instName = it.key();
               string socket = it.value().at("unix_socket_path");
               string hostname = it.value().at("hostname");
               int port = it.value().at("port");
               inst_entry[instName] = {socket, hostname, port};
            }

            for (auto it = j["DATABASES"].begin(); it!= j["DATABASES"].end(); it++)
            {
               string dbName = it.key();
               string instName = it.value().at("instance");
               int dbId = it.value().at("id");
               string separator = it.value().at("separator");
               db_entry[dbName] = {instName, dbId, separator};

               separator_entry.emplace(dbId, separator);
            }
        }

        catch (domain_error& e)
        {
            SWSS_LOG_ERROR("key doesn't exist in json object, NULL value has no iterator >> %s\n", e.what());
            throw runtime_error("key doesn't exist in json object, NULL value has no iterator >> " + string(e.what()));
        }
        catch (exception &e)
        {
            SWSS_LOG_ERROR("Sonic database config file syntax error >> %s\n", e.what());
            throw runtime_error("Sonic database config file syntax error >> " + string(e.what()));
        }
    }
    else
    {
        SWSS_LOG_ERROR("Sonic database config file doesn't exist at %s\n", file.c_str());
        throw runtime_error("Sonic database config file doesn't exist at " + file);
    }
}

void SonicDBConfig::initializeGlobalConfig(const string &file)
{
    std::string local_file, dir_name, ns_name;
    std::unordered_map<std::string, SonicDBInfo> db_entry;
    std::unordered_map<std::string, RedisInstInfo> inst_entry;
    std::unordered_map<int, std::string> separator_entry;

    SWSS_LOG_ENTER();

    if (m_global_init)
    {
        SWSS_LOG_ERROR("SonicDBConfig Global config is already initialized");
        return;
    }

    ifstream i(file);
    if (i.good())
    {
        local_file = dir_name = std::string();

        // Get the directory name from the file path given as input.
        std::string::size_type pos = file.rfind("/");
        if( pos != std::string::npos)
        {
            dir_name = file.substr(0,pos+1);
        }

        try
        {
            json j;
            i >> j;

            for (auto& element : j["INCLUDES"])
            {
                local_file.append(dir_name);
                local_file.append(element["include"]);

                if(element["namespace"].empty())
                {
                    // If database_config.json is already initlized via SonicDBConfig::initialize
                    // skip initializing it here again.
                    if(m_init)
                    {
                        local_file.clear();
                        continue;
                    }
                    ns_name = EMPTY_NAMESPACE;
                }
                else
                {
                    ns_name = element["namespace"];
                }

                parseDatabaseConfig(local_file, inst_entry, db_entry, separator_entry);
                m_inst_info[ns_name] = inst_entry;
                m_db_info[ns_name] = db_entry;
                m_db_separator[ns_name] = separator_entry;

                if(element["namespace"].empty())
                {
                    // Make regular init also done
                    m_init = true;
                }

                inst_entry.clear();
                db_entry.clear();
                separator_entry.clear();
                local_file.clear();
            }
        }

        catch (domain_error& e)
        {
            SWSS_LOG_ERROR("key doesn't exist in json object, NULL value has no iterator >> %s\n", e.what());
            throw runtime_error("key doesn't exist in json object, NULL value has no iterator >> " + string(e.what()));
        }
        catch (exception &e)
        {
            SWSS_LOG_ERROR("Sonic database config file syntax error >> %s\n", e.what());
            throw runtime_error("Sonic database config file syntax error >> " + string(e.what()));
        }
    }
    else
    {
        SWSS_LOG_ERROR("Sonic database config global file doesn't exist at %s\n", file.c_str());
    }

    // Set it as the global config file is already parsed and init done.
    m_global_init = true;
}

void SonicDBConfig::initialize(const string &file)
{
    std::unordered_map<std::string, SonicDBInfo> db_entry;
    std::unordered_map<std::string, RedisInstInfo> inst_entry;
    std::unordered_map<int, std::string> separator_entry;

    SWSS_LOG_ENTER();

    if (m_init)
    {
        SWSS_LOG_ERROR("SonicDBConfig already initialized");
        throw runtime_error("SonicDBConfig already initialized");
    }

    parseDatabaseConfig(file, inst_entry, db_entry, separator_entry);
    m_inst_info[EMPTY_NAMESPACE] = inst_entry;
    m_db_info[EMPTY_NAMESPACE] = db_entry;
    m_db_separator[EMPTY_NAMESPACE] = separator_entry;

    // Set it as the config file is already parsed and init done.
    m_init = true;
}

void SonicDBConfig::validateNamespace(const string &netns)
{
    SWSS_LOG_ENTER();

    // With valid namespace input and database_global.json is not loaded, ask user to initializeGlobalConfig first
    if(!netns.empty())
    {
        // If global initialization is not done, ask user to initialize global DB Config first.
        if (!m_global_init)
        {
            SWSS_LOG_THROW("Initialize global DB config using API SonicDBConfig::initializeGlobalConfig");
        }

        // Check if the namespace is valid, check if this is a key in either of this map
        unordered_map<string, unordered_map<string, RedisInstInfo>>::const_iterator entry = m_inst_info.find(netns);
        if (entry == m_inst_info.end())
        {
            SWSS_LOG_THROW("Namespace %s is not a valid namespace name in config file", netns.c_str());
        }
    }
}

SonicDBInfo& SonicDBConfig::getDbInfo(const std::string &dbName, const std::string &netns)
{
    SWSS_LOG_ENTER();

    if (!m_init)
        initialize(DEFAULT_SONIC_DB_CONFIG_FILE);

    if (!netns.empty())
    {
        if (!m_global_init)
        {
            SWSS_LOG_THROW("Initialize global DB config using API SonicDBConfig::initializeGlobalConfig");
        }
    }
    auto foundNetns = m_db_info.find(netns);
    if (foundNetns == m_db_info.end())
    {
        string msg = "Namespace " + netns + " is not a valid namespace name in config file";
        SWSS_LOG_ERROR("%s", msg.c_str());
        throw out_of_range(msg);
    }
    auto& infos = foundNetns->second;
    auto foundDb = infos.find(dbName);
    if (foundDb == infos.end())
    {
        string msg = "Failed to find " + dbName + " database in " + netns + " namespace";
        SWSS_LOG_ERROR("%s", msg.c_str());
        throw out_of_range(msg);
    }
    return foundDb->second;
}

RedisInstInfo& SonicDBConfig::getRedisInfo(const std::string &dbName, const std::string &netns)
{
    SWSS_LOG_ENTER();

    if (!m_init)
        initialize(DEFAULT_SONIC_DB_CONFIG_FILE);

    if (!netns.empty())
    {
        if (!m_global_init)
        {
            SWSS_LOG_THROW("Initialize global DB config using API SonicDBConfig::initializeGlobalConfig");
        }
    }
    auto foundNetns = m_inst_info.find(netns);
    if (foundNetns == m_inst_info.end())
    {
        string msg = "Namespace " + netns + " is not a valid namespace name in Redis instances in config file";
        SWSS_LOG_ERROR("%s", msg.c_str());
        throw out_of_range(msg);
    }
    auto& infos = foundNetns->second;
    auto foundRedis = infos.find(getDbInst(dbName));
    if (foundRedis == infos.end())
    {
        string msg = "Failed to find the Redis instance for " + dbName + " database in " + netns + " namespace";
        SWSS_LOG_ERROR("%s", msg.c_str());
        throw out_of_range(msg);
    }
    return foundRedis->second;
}

string SonicDBConfig::getDbInst(const string &dbName, const string &netns)
{
    return getDbInfo(dbName, netns).instName;
}

int SonicDBConfig::getDbId(const string &dbName, const string &netns)
{
    return getDbInfo(dbName, netns).dbId;
}

string SonicDBConfig::getSeparator(const string &dbName, const string &netns)
{
    return getDbInfo(dbName, netns).separator;
}

string SonicDBConfig::getSeparator(int dbId, const string &netns)
{
    if (!m_init)
        initialize(DEFAULT_SONIC_DB_CONFIG_FILE);
    auto foundNetns = m_db_separator.find(netns);
    if (foundNetns == m_db_separator.end())
    {
        string msg = "Namespace " + netns + " is not a valid namespace name in config file";
        SWSS_LOG_ERROR("%s", msg.c_str());
        throw out_of_range(msg);
    }
    auto seps = foundNetns->second;
    auto foundDb = seps.find(dbId);
    if (foundDb == seps.end())
    {
        string msg = "Failed to find " + to_string(dbId) + " database in " + netns + " namespace";
        SWSS_LOG_ERROR("%s", msg.c_str());
        throw out_of_range(msg);
    }
    return foundDb->second;
}

string SonicDBConfig::getSeparator(const DBConnector* db)
{
    if (!db)
    {
        throw std::invalid_argument("db cannot be null");
    }

    string dbName = db->getDbName();
    string netns = db->getNamespace();
    if (dbName.empty())
    {
        return getSeparator(db->getDbId(), netns);
    }
    else
    {
        return getSeparator(dbName, netns);
    }
}

string SonicDBConfig::getDbSock(const string &dbName, const string &netns)
{
    return getRedisInfo(dbName, netns).unixSocketPath;
}

string SonicDBConfig::getDbHostname(const string &dbName, const string &netns)
{
    return getRedisInfo(dbName, netns).hostname;
}

int SonicDBConfig::getDbPort(const string &dbName, const string &netns)
{
    return getRedisInfo(dbName, netns).port;
}

vector<string> SonicDBConfig::getNamespaces()
{
    vector<string> list;

    if (!m_global_init)
        initializeGlobalConfig();

    // This API returns back non-empty namespaces.
    for (auto it = m_inst_info.cbegin(); it != m_inst_info.cend(); ++it) {
        if(!((it->first).empty()))
            list.push_back(it->first);
    }

    return list;
}

constexpr const char *SonicDBConfig::DEFAULT_SONIC_DB_CONFIG_FILE;
constexpr const char *SonicDBConfig::DEFAULT_SONIC_DB_GLOBAL_CONFIG_FILE;
unordered_map<string, unordered_map<string, RedisInstInfo>> SonicDBConfig::m_inst_info;
unordered_map<string, unordered_map<string, SonicDBInfo>> SonicDBConfig::m_db_info;
unordered_map<string, unordered_map<int, string>> SonicDBConfig::m_db_separator;
bool SonicDBConfig::m_init = false;
bool SonicDBConfig::m_global_init = false;

constexpr const char *DBConnector::DEFAULT_UNIXSOCKET;

void DBConnector::select(DBConnector *db)
{
    string select("SELECT ");
    select += to_string(db->getDbId());

    RedisReply r(db, select, REDIS_REPLY_STATUS);
    r.checkStatusOK();
}

DBConnector::~DBConnector()
{
    redisFree(m_conn);
}

DBConnector::DBConnector(int dbId, const string& hostname, int port,
                         unsigned int timeout) :
    m_dbId(dbId),
    m_namespace(EMPTY_NAMESPACE)
{
    struct timeval tv = {0, (suseconds_t)timeout * 1000};

    if (timeout)
        m_conn = redisConnectWithTimeout(hostname.c_str(), port, tv);
    else
        m_conn = redisConnect(hostname.c_str(), port);

    if (m_conn->err)
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect to redis");

    select(this);
}

DBConnector::DBConnector(int dbId, const string& unixPath, unsigned int timeout) :
    m_dbId(dbId),
    m_namespace(EMPTY_NAMESPACE)
{
    struct timeval tv = {0, (suseconds_t)timeout * 1000};

    if (timeout)
        m_conn = redisConnectUnixWithTimeout(unixPath.c_str(), tv);
    else
        m_conn = redisConnectUnix(unixPath.c_str());

    if (m_conn->err)
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect to redis (unix-socket)");

    select(this);
}

DBConnector::DBConnector(const string& dbName, unsigned int timeout, bool isTcpConn, const string& netns)
    : m_dbId(SonicDBConfig::getDbId(dbName, netns))
    , m_dbName(dbName)
    , m_namespace(netns)
{
    struct timeval tv = {0, (suseconds_t)timeout * 1000};

    if (timeout)
    {
        if (isTcpConn)
            m_conn = redisConnectWithTimeout(SonicDBConfig::getDbHostname(dbName, netns).c_str(), SonicDBConfig::getDbPort(dbName, netns), tv);
        else
            m_conn = redisConnectUnixWithTimeout(SonicDBConfig::getDbSock(dbName, netns).c_str(), tv);
    }
    else
    {
        if (isTcpConn)
            m_conn = redisConnect(SonicDBConfig::getDbHostname(dbName, netns).c_str(), SonicDBConfig::getDbPort(dbName, netns));
        else
            m_conn = redisConnectUnix(SonicDBConfig::getDbSock(dbName, netns).c_str());
    }

    if (m_conn->err)
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect to redis");

    select(this);
}

DBConnector::DBConnector(const string& dbName, unsigned int timeout, bool isTcpConn)
    : DBConnector(dbName, timeout, isTcpConn, EMPTY_NAMESPACE)
{
    // Empty contructor
}

redisContext *DBConnector::getContext() const
{
    return m_conn;
}

int DBConnector::getDbId() const
{
    return m_dbId;
}

string DBConnector::getDbName() const
{
    return m_dbName;
}

string DBConnector::getNamespace() const
{
    return m_namespace;
}

DBConnector *DBConnector::newConnector(unsigned int timeout) const
{
    DBConnector *ret;

    if (getContext()->connection_type == REDIS_CONN_TCP)
        ret = new DBConnector(getDbId(),
                               getContext()->tcp.host,
                               getContext()->tcp.port,
                               timeout);
    else
        ret = new DBConnector(getDbId(),
                               getContext()->unix_sock.path,
                               timeout);

    ret->m_dbName = m_dbName;
    ret->m_namespace = m_namespace;

    return ret;
}

void DBConnector::setClientName(const string& clientName)
{
    string command("CLIENT SETNAME ");
    command += clientName;

    RedisReply r(this, command, REDIS_REPLY_STATUS);
    r.checkStatusOK();
}

string DBConnector::getClientName()
{
    string command("CLIENT GETNAME");

    RedisReply r(this, command);

    auto ctx = r.getContext();
    if (ctx->type == REDIS_REPLY_STRING)
    {
        return r.getReply<std::string>();
    }
    else
    {
        if (ctx->type != REDIS_REPLY_NIL)
            SWSS_LOG_ERROR("Unable to obtain Redis client name");

        return "";
    }
}

}
