#include <iostream>
#include <iomanip>
#include <vector>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include "schema.h"
#include "logger.h"
#include "dbconnector.h"
#include "redisclient.h"
#include "producerstatetable.h"

using namespace swss;

[[ noreturn ]] void usage(std::string program, int status, std::string message)
{
    if (message.size() != 0)
    {
        std::cout << message << std::endl << std::endl;
    }

    std::cout << "Usage: " << program << " [OPTIONS]" << std::endl
              << "SONiC logging severity level setting." << std::endl << std::endl
              << "Options:" << std::endl
              << "\t -h\tprint this message" << std::endl
              << "\t -l\tloglevel value" << std::endl
              << "\t -c\tcomponent name in DB for which loglevel is applied (provided with -l)" << std::endl
              << "\t -a\tapply loglevel to all components (provided with -l)" << std::endl
              << "\t -s\tapply loglevel for SAI api component (equivalent to adding prefix \"SAI_API_\" to component)" << std::endl
              << "\t -p\tprint components registered in DB for which setting can be applied" << std::endl << std::endl
              << "Examples:" << std::endl
              << "\t" << program << " -l NOTICE -c orchagent # set severity level " << std::endl
              << "\t" << program << " -l SAI_LOG_ERROR -s -c SWITCH # set SAI_API_SWITCH severity " << std::endl;

    exit(status);
}

void setLoglevel(DBConnector& db, const std::string& component, const std::string& loglevel)
{
    ProducerStateTable table(&db, component);
    FieldValueTuple fv(DAEMON_LOGLEVEL, loglevel);
    std::vector<FieldValueTuple>fieldValues = { fv };
    table.set(component, fieldValues);
}

bool validateSaiLoglevel(std::string prioStr)
{
    static const std::vector<std::string> saiPrios = {
        "SAI_LOG_LEVEL_CRITICAL",
        "SAI_LOG_LEVEL_ERROR",
        "SAI_LOG_LEVEL_WARN",
        "SAI_LOG_LEVEL_NOTICE",
        "SAI_LOG_LEVEL_INFO",
        "SAI_LOG_LEVEL_DEBUG",
    };

    return std::find(saiPrios.begin(), saiPrios.end(), prioStr) != saiPrios.end();
}

bool filterOutKeysets(const std::string& key)
{
    return key.find("_KEY_SET") != std::string::npos;
}

bool filterOutSaiKeys(const std::string& key)
{
    return key.find("SAI_API_") != std::string::npos;
}

bool filterSaiKeys(const std::string& key)
{
    return key.find("SAI_API_") == std::string::npos;
}

int main(int argc, char **argv)
{
    int opt;
    bool applyToAll = false, print = false;
    std::string prefix = "", component, loglevel;
    auto exitWithUsage = std::bind(usage, argv[0], std::placeholders::_1, std::placeholders::_2);

    while ((opt = getopt (argc, argv, "c:l:saph")) != -1)
    {
        switch(opt)
        {
            case 'c':
                component = optarg;
                break;
            case 'l':
                loglevel = optarg;
                break;
            case 's':
                prefix = "SAI_API_";
                break;
            case 'a':
                applyToAll = true;
                break;
            case 'p':
                print = true;
                break;
            case 'h':
                exitWithUsage(EXIT_SUCCESS, "");
                break;
            default:
                exitWithUsage(EXIT_FAILURE, "Invalid option");
        }
    }

    DBConnector db(LOGLEVEL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisClient redisClient(&db);
    auto keys = redisClient.keys("*");
    for (auto& key : keys)
    {
        size_t colonPos = key.find(":");
        if (colonPos == std::string::npos)
        {
            continue;
        }

        key = key.substr(0, colonPos);
    }
    // Ignore autogenerated keysets
    keys.erase(std::remove_if(keys.begin(), keys.end(), filterOutKeysets), keys.end());

    if (print)
    {
        if (argc != 2)
        {
            exitWithUsage(EXIT_FAILURE, "-p option does not accept other options");
        }

        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys)
        {
            auto level = redisClient.hget(key + ":" + key, DAEMON_LOGLEVEL);
            std::cout << std::left << std::setw(30) << key << *level << std::endl;
        }
        return (EXIT_SUCCESS);
    }

    if ((prefix == "SAI_API_") && !validateSaiLoglevel(loglevel))
    {
        exitWithUsage(EXIT_FAILURE, "Invalid SAI loglevel value");
    }
    else if ((prefix == "") && (Logger::priorityStringMap.find(loglevel) == Logger::priorityStringMap.end()))
    {
        exitWithUsage(EXIT_FAILURE, "Invalid loglevel value");
    }

    if (applyToAll)
    {
        if (component != "")
        {
            exitWithUsage(EXIT_FAILURE, "Invalid options provided with -a");
        }

        if (prefix == "SAI_API_")
        {
            keys.erase(std::remove_if(keys.begin(), keys.end(), filterSaiKeys), keys.end());
        }
        else
        {
            keys.erase(std::remove_if(keys.begin(), keys.end(), filterOutSaiKeys), keys.end());
        }

        for (const auto& key : keys)
        {
            setLoglevel(db, key, loglevel);
        }

        exit(EXIT_SUCCESS);
    }

    component = prefix + component;
    if (std::find(std::begin(keys), std::end(keys), component) == keys.end())
    {
        exitWithUsage(EXIT_FAILURE, "Component not present in DB");
    }

    setLoglevel(db, component, loglevel);

    return EXIT_SUCCESS;
}
