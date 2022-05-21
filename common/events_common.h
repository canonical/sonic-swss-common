#ifndef _EVENTS_COMMON_H
#define _EVENTS_COMMON_H
/*
 * common APIs used by events code.
 */
#include <stdio.h>
#include <chrono>
#include <fstream>
#include <thread>
#include <errno.h>
#include "string.h"
#include "json.hpp"
#include "zmq.h"
#include <unordered_map>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/map.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

#include "logger.h"
#include "events.h"

using namespace std;
using namespace chrono;

extern int errno;
extern int zerrno;

/*
 * Max count of possible concurrent event publishers
 * A rough estimate only, more as a guideline than strict.
         SWSS_LOG_ERROR(fmt.c_str(), e, zerrno __VA_OPT__(,) __VA_ARGS__); \
 * So this does not limit any usage
 */
#define MAX_PUBLISHERS_COUNT  1000


/* TODO: Combine two SWSS_LOG_ERROR into one */
#define RET_ON_ERR(res, msg, ...)\
    if (!(res)) {\
        int e = errno; \
        zerrno = zmq_errno(); \
        SWSS_LOG_ERROR(msg, ##__VA_ARGS__); \
        SWSS_LOG_ERROR("last:errno=%d zerr=%d", e, zerrno); \
        goto out; }

#define ERR_CHECK(res, ...) {\
    if (!(res)) \
        SWSS_LOG_ERROR(__VA_ARGS__); }

// Some simple definitions
//
typedef map<string, string> map_str_str_t;

/*
 * Config that can be read from init_cfg
 */
#define INIT_CFG_PATH "/etc/sonic/init_cfg.json"
#define CFG_EVENTS_KEY "events"

/* configurable entities' keys */
#define XSUB_END_KEY "xsub_path"
#define XPUB_END_KEY "xpub_path"
#define REQ_REP_END_KEY "req_rep_path"
#define CAPTURE_END_KEY "capture_path"
#define STATS_UPD_SECS "stats_upd_secs"
#define CACHE_MAX_CNT "cache_max_cnt"

/* init config from file */
void read_init_config(const char *fname);

/* Read config entry for a key */
string get_config(const string key);

template<typename T>
T get_config_data(const string key, T def)
{
    string s(get_config(key));

    if (s.empty()) {
        return def;
    }
    else {
        T v;
        stringstream ss(s);

        ss >> v;

        return v;
    }
}


const string get_timestamp();

/*
 * Way to serialize map or vector
 * boost::archive::text_oarchive could be used to archive any struct/class
 * but that class needs some additional support, that declares
 * boost::serialization::access as private friend and couple more tweaks
 * std::map inherently supports serialization
 */
template <typename Map>
const string
serialize(const Map& data)
{
    std::stringstream ss;
    boost::archive::text_oarchive oarch(ss);
    oarch << data;
    return ss.str();
}

template <typename Map>
void
deserialize(const string& s, Map& data)
{
    std::stringstream ss;
    ss << s;
    boost::archive::text_iarchive iarch(ss);
    iarch >> data;
    return;
}


template <typename Map>
int
map_to_zmsg(const Map& data, zmq_msg_t &msg)
{
    string s = serialize(data);

    int rc = zmq_msg_init_size(&msg, s.size());
    if (rc == 0) {
        strncpy((char *)zmq_msg_data(&msg), s.c_str(), s.size());
    }
    return rc;
}


template <typename Map>
void
zmsg_to_map(zmq_msg_t &msg, Map& data)
{
    string s((const char *)zmq_msg_data(&msg), zmq_msg_size(&msg));
    deserialize(s, data);
}


/*
 * events are published as two part zmq message.
 * First part only has the event source, so receivers could
 * filter by source.
 *
 * Second part contains map as defined in internal_event_t.
 * The map is serialized and sent as string.
 *
 */
/*
 * This is data going over wire and using cache. So be conservative
 * on names
 */
#define EVENT_STR_DATA "d"
#define EVENT_RUNTIME_ID  "r"
#define EVENT_SEQUENCE "s"

typedef map<string, string> internal_event_t;

/* Sequence is converted to string in message */
typedef uint32_t sequence_t;
typedef string runtime_id_t;

internal_event_t internal_event_ref = {
    { EVENT_STR_DATA, "" },
    { EVENT_RUNTIME_ID, "" },
    { EVENT_SEQUENCE, "" } };

/* ZMQ message part 2 contains serialized version of internal_event_t */
typedef string events_data_type_t;
typedef vector<events_data_type_t> events_data_lst_t;


template<typename DT>
int 
zmq_read_part(void *sock, int flag, int &more, DT data)
{
    zmq_msg_t msg;

    more = 0;
    zmq_msg_init(&msg);
    int rc = zmq_msg_recv(&msg, sock, flag);

    if (rc != -1) {
        size_t more_size = sizeof (more);

        zmsg_to_map(msg, data);

        rc = zmq_getsockopt (sock, ZMQ_RCVMORE, &more, &more_size);

    }
    zmq_msg_close(&msg);

    return rc;
}

   
template<typename DT>
int
zmq_send_part(void *sock, int flag, DT data)
{
    zmq_msg_t msg;

    int rc = map_to_zmsg(data, msg);
    RET_ON_ERR(rc == 0, "Failed to map to zmsg %d", 5);

    rc = zmq_msg_send (&msg, sock, flag);
    RET_ON_ERR(rc != -1, "Failed to send part %d", 5);

    rc = 0;
out:
    zmq_msg_close(&msg);
    return rc;
}

template<typename P1, typename P2>
int
zmq_message_send(void *sock, P1 pt1, P2 pt2)
{
    int rc = zmq_send_part(sock, pt2.empty() ? 0 : ZMQ_SNDMORE, pt1);

    if ((rc == 0) && (!pt2.empty())) {
        rc = zmq_send_part(sock, 0, pt2);
    }
    return rc;
}

   
template<typename P1, typename P2>
int
zmq_message_read(void *sock, int flag, P1 pt1, P2 pt2)
{
    int more = 0, rc;
    
    rc = zmq_read_part(sock, flag, more, pt1);
    RET_ON_ERR(rc == 0, "Failed to read part1");

    if (more) {
        rc = zmq_read_part(sock, 0, more, pt2);
        RET_ON_ERR(rc == 0, "Failed to read part1");
        RET_ON_ERR(!more, "Don't expect more than 2 parts");
    }

out:
    return rc;
}

#endif /* !_EVENTS_COMMON_H */ 
