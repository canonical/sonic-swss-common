#include <iostream>
#include <memory>
#include <thread>
#include <algorithm>
#include <deque>
#include "gtest/gtest.h"
#include "common/dbconnector.h"
#include "common/notificationconsumer.h"
#include "common/notificationproducer.h"
#include "common/select.h"
#include "common/selectableevent.h"
#include "common/table.h"
#include "common/shmproducerstatetable.h"
#include "common/shmconsumerstatetable.h"

using namespace std;
using namespace swss;

#define TEST_DB           "APPL_DB" // Need to test against a DB which uses a colon table name separator due to hardcoding in consumer_table_pops.lua
#define NUMBER_OF_THREADS    (10) // Spawning more than 256 threads causes libc++ to except
#define NUMBER_OF_OPS        (100)
#define MAX_FIELDS       10 // Testing up to 30 fields objects
#define PRINT_SKIP           (10) // Print + for Producer and - for Consumer for every 100 ops
#define MAX_KEYS             (10)       // Testing up to 30 keys objects

static int running_thread_count = 0;

static inline string field(int i)
{
    return string("field ") + to_string(i);
}

static inline string value(int i)
{
    if (i == 0) return string(); // empty
    return string("value ") + to_string(i);
}

static inline bool IsDigit(char ch)
{
    return (ch >= '0') && (ch <= '9');
}

static inline int readNumberAtEOL(const string& str)
{
    if (str.empty()) return 0;
    auto pos = find_if(str.begin(), str.end(), IsDigit);
    istringstream is(str.substr(pos - str.begin()));
    int ret;
    is >> ret;
    EXPECT_TRUE((bool)is);
    return ret;
}

static void producerWorker(string tableName)
{
    DBConnector db(TEST_DB, 0, true);
    ShmProducerStateTable p(&db, tableName);
    cout << "Thread started: " << tableName << endl;

    for (int i = 0; i < NUMBER_OF_OPS; i++)
    {
        vector<FieldValueTuple> fields;
        for (int j = 0; j < MAX_FIELDS; j++)
        {
            FieldValueTuple t(field(j), value(j));
            fields.push_back(t);
        }
        p.set("set_key_" + to_string(i), fields);
    }

    for (int i = 0; i < NUMBER_OF_OPS; i++)
    {
        p.del("del_key_" + to_string(i));
    }

    for (int i = 0; i < NUMBER_OF_OPS; i++)
    {
        std::vector<KeyOpFieldsValuesTuple> vkco;
        vkco.resize(MAX_KEYS);
        for (int j = 0; j < MAX_KEYS; j++)
        {
            auto& kco = vkco[j];
            auto& values = kfvFieldsValues(kco);
            kfvKey(kco) = "batch_set_key_" + to_string(i) + "_" + to_string(j);
            kfvOp(kco) = "SET";
            
            for (int k = 0; k < MAX_FIELDS; k++)
            {
                FieldValueTuple t(field(k), value(k));
                values.push_back(t);
            }
        }
        
        cout << "Send event: " << "batch_set_key_" + to_string(i) << endl;
        p.set(vkco);
    }

    for (int i = 0; i < NUMBER_OF_OPS; i++)
    {
        std::vector<std::string> keys;
        for (int j = 0; j < MAX_KEYS; j++)
        {
            keys.push_back("batch_del_key_" + to_string(i) + "_" + to_string(j));
        }

        cout << "Send event: " << "batch_del_key_" + to_string(i) << endl;
        p.del(keys);
    }

    cout << "Thread ended: " << tableName << endl;
    
    running_thread_count--;
}

// variable used by consumer worker
static int setCount = 0;
static int delCount = 0;
static int batchSetCount = 0;
static int batchDelCount = 0;
    
static void consumerWorker(string tableName)
{
    cout << "Consumer thread started: " << tableName << endl;
    
    DBConnector db(TEST_DB, 0, true);
    ShmConsumerStateTable c(&db, tableName);
    Select cs;
    cs.addSelectable(&c);

    //validate received data
    Selectable *selectcs;
    std::deque<KeyOpFieldsValuesTuple> vkco;
    int ret = 0;
    int wait_loop_count = 10;
    while (running_thread_count > 0 || wait_loop_count > 0)
    {
        if (running_thread_count <= 0)
        {
            // after all producer thread exit, maybe still some event in queue
            wait_loop_count--;
        }

        while ((ret = cs.select(&selectcs)) == Select::OBJECT)
        {
            c.pops(vkco);
            while (!vkco.empty())
            {
                auto &kco = vkco.front();
                auto key = kfvKey(kco);
                auto op = kfvOp(kco);
                auto fvs = kfvFieldsValues(kco);

                if (key.rfind("batch_set_key_", 0) == 0)
                {
                    EXPECT_EQ(fvs.size(), MAX_FIELDS);
                    batchSetCount++;
                }
                else if (key.rfind("set_key_", 0) == 0)
                {
                    EXPECT_EQ(fvs.size(), MAX_FIELDS);
                    setCount++;
                }
                else if (key.rfind("batch_del_key_", 0) == 0)
                {
                    EXPECT_EQ(fvs.size(), 0);
                    batchDelCount++;
                }
                else if (key.rfind("del_key_", 0) == 0)
                {
                    EXPECT_EQ(fvs.size(), 0);
                    delCount++;
                }

                vkco.pop_front();
            }
        }

        sleep(1);
    }

    cout << "Consumer thread ended: " << tableName << endl;
}

TEST(ShmConsumerStateTable, test)
{
    std::string testTableName = "SHM_PROD_CONS_UT";
    thread *producerThreads[NUMBER_OF_THREADS];

    // reset msg queue before test
    auto queueName = ShmConsumerStateTable::GetQueueName(TEST_DB, testTableName);
    ShmConsumerStateTable::TryRemoveShmQueue(queueName);

    // start consumer first, SHM can only have 1 consumer per table.
    thread *consumerThread = new thread(consumerWorker, testTableName);

    cout << "Starting " << NUMBER_OF_THREADS << " producers" << endl;
    /* Starting the producer before the producer */
    for (int i = 0; i < NUMBER_OF_THREADS; i++)
    {
        running_thread_count++;
        producerThreads[i] = new thread(producerWorker, testTableName);
    }

    cout << "Done. Waiting for all job to finish " << NUMBER_OF_OPS << " jobs." << endl;
    for (int i = 0; i < NUMBER_OF_THREADS; i++)
    {
        producerThreads[i]->join();
        delete producerThreads[i];
    }
    
    consumerThread->join();
    delete consumerThread;

    EXPECT_EQ(setCount, NUMBER_OF_THREADS * NUMBER_OF_OPS);
    EXPECT_EQ(delCount, NUMBER_OF_THREADS * NUMBER_OF_OPS);
    EXPECT_EQ(batchSetCount, NUMBER_OF_THREADS * NUMBER_OF_OPS * MAX_KEYS);
    EXPECT_EQ(batchDelCount, NUMBER_OF_THREADS * NUMBER_OF_OPS * MAX_KEYS);

    cout << endl << "Done." << endl;
}