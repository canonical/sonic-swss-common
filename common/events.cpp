#include "events_pi.h"
#include "events_wrap.h"

/*
 *  Track created publishers to avoid duplicates
 *  As receivers track missed event count by publishing instances, avoiding
 *  duplicates helps reduce load.
 */

lst_publishers_t EventPublisher::s_publishers;

event_handle_t
EventPublisher::get_publisher(const string event_source)
{
    event_handle_t ret = NULL;
    lst_publishers_t::const_iterator itc = s_publishers.find(event_source);
    if (itc != s_publishers.end()) {
        // Pre-exists
        ret = itc->second.get();
    }
    else {
        EventPublisher_ptr_t p(new EventPublisher());

        int rc = p->init(event_source);

        if (rc == 0) {
            ret = p.get();
            s_publishers[event_source] = p;
        }
    }
    return ret;
}

void
EventPublisher::drop_publisher(event_handle_t handle)
{
    lst_publishers_t::iterator it;

    for(it=s_publishers.begin(); it != s_publishers.end(); ++it) {
        if (it->second.get() == handle) {
            s_publishers.erase(it);
            break;
        }
    }
}

int
EventPublisher::do_publish(event_handle_t handle, const string tag, const event_params_t *params)
{
    lst_publishers_t::const_iterator itc;
    for(itc=s_publishers.begin(); itc != s_publishers.end(); ++itc) {
        if (itc->second.get() == handle) {
            return itc->second->publish(tag, params);
        }
    }
    return -1;
}


EventPublisher::EventPublisher(): m_zmq_ctx(NULL), m_socket(NULL), m_sequence(0)
{}

static string
get_uuid()
{
    uuid_t id;
    char uuid_str[UUID_STR_SIZE];
    uuid_generate(id);
    uuid_unparse(id, uuid_str);
    return string(uuid_str);
}

int EventPublisher::init(const string event_source)
{
    m_zmq_ctx = zmq_ctx_new();
    void *sock = zmq_socket (m_zmq_ctx, ZMQ_PUB);

    int rc = zmq_connect (sock, get_config(XSUB_END_KEY).c_str());
    RET_ON_ERR(rc == 0, "Publisher fails to connect %s", get_config(XSUB_END_KEY).c_str());

    /*
     * Event service could be down. So have a timeout.
     * 
     */
    rc = m_event_service.init_client(m_zmq_ctx, EVENTS_SERVICE_TIMEOUT_MS_PUB);
    RET_ON_ERR (rc == 0, "Failed to init event service");

    /*
     * REQ socket is connected and a message is sent & received, more to
     * ensure PUB socket had enough time to establish connection.
     * Any message published before connection establishment is dropped.
     * NOTE: We don't wait for response here, but read it upon first publish
     * If the publisher init happened early at the start by caller, by the
     * the first event is published, the echo response will be available locally.
     */
    rc = m_event_service.echo_send("hello");
    RET_ON_ERR (rc == 0, "Failed to echo send in event service");

    m_event_source = event_source;

    m_socket = sock;
out:
    if (m_socket == NULL) {
        zmq_close(sock);
    }
    return rc;
}

EventPublisher::~EventPublisher()
{
    if (!m_runtime_id.empty()) {
        /* Retire the runtime ID */
        send_evt(EVENT_STR_CTRL_DEINIT);
    }
    m_event_service.close_service();
    if (m_socket != NULL) {
        zmq_close(m_socket);
    }
    zmq_ctx_term(m_zmq_ctx);
}


int
EventPublisher::send_evt(const string str_data)
{
    internal_event_t event_data;
    int rc;
    stringstream ss;

    auto timepoint = system_clock::now();
    ss << duration_cast<microseconds>(timepoint.time_since_epoch()).count();

    event_data[EVENT_STR_DATA] = str_data;
    event_data[EVENT_RUNTIME_ID] = m_runtime_id;
    /* A value of 0 will indicate rollover */
    ++m_sequence;
    event_data[EVENT_SEQUENCE] = seq_to_str(m_sequence);
    event_data[EVENT_EPOCH] = ss.str();

    rc = zmq_message_send(m_socket, m_event_source, event_data);
    RET_ON_ERR(rc == 0, "failed to send for tag %s", str_data.substr(0, 20).c_str());
out:
    return rc;
}


int
EventPublisher::publish(const string tag, const event_params_t *params)
{
    int rc;
    string str_data;

    if (m_event_service.is_active()) {
        string s;

        /* Failure is no-op; The eventd service my be down
         * NOTE: This call atmost blocks for EVENTS_SERVICE_TIMEOUT_MS_PUB
         * as provided in publisher init.
         */
        m_event_service.echo_receive(s);

        /* Close it as we don't need it anymore */
        m_event_service.close_service();
    }

    if (m_runtime_id.empty()) {
        m_runtime_id = get_uuid();
    }

    /* Check for timestamp in params. If not, provide it. */
    string param_str;
    event_params_t evt_params;
    if (params != NULL) {
        if (params->find(EVENT_TS_PARAM) == params->end()) {
            evt_params = *params;
            evt_params[EVENT_TS_PARAM] = get_timestamp();
            params = &evt_params;
        }
    }
    else {
        evt_params[EVENT_TS_PARAM] = get_timestamp();
        params = &evt_params;
    }

    str_data = convert_to_json(m_event_source + ":" + tag, *params);
    rc = send_evt(str_data);
    RET_ON_ERR(rc == 0, "failed to send event str[%d]= %s", (int)str_data.size(),
        str_data.substr(0, 20).c_str());

    SWSS_LOG_INFO("EVENT_PUBLISHED: %s", str_data.sunbstr(0, 80).c_str());

out:
    return rc;
}

event_handle_t
events_init_publisher(const string event_source)
{
    return EventPublisher::get_publisher(event_source);
}

void
events_deinit_publisher(event_handle_t handle)
{
    EventPublisher::drop_publisher(handle);
}

int
event_publish(event_handle_t handle, const string tag, const event_params_t *params)
{
    return EventPublisher::do_publish(handle, tag, params);
}


/* Expect only one subscriber per process */
EventSubscriber_ptr_t EventSubscriber::s_subscriber;

event_handle_t
EventSubscriber::get_subscriber(bool use_cache, int recv_timeout,
        const event_subscribe_sources_t *sources)
{

    if (s_subscriber == NULL) {
        EventSubscriber_ptr_t sub(new EventSubscriber());

        RET_ON_ERR(sub->init(use_cache, recv_timeout, sources) == 0,
                "Failed to init subscriber");

        s_subscriber = sub;
    }
out:
    return s_subscriber.get();
}


void
EventSubscriber::drop_subscriber(event_handle_t handle)
{
    if ((handle == s_subscriber.get()) && (s_subscriber != NULL)) {
        s_subscriber.reset();
    }
}


event_receive_op_t
EventSubscriber::do_receive(event_handle_t handle)
{
    event_receive_op_t op;

    if ((handle == s_subscriber.get()) && (s_subscriber != NULL)) {
        op = s_subscriber->event_receive();
    }
    return op;
}

EventSubscriber::EventSubscriber(): m_zmq_ctx(NULL), m_socket(NULL),
    m_cache_read(false)
{};


EventSubscriber::~EventSubscriber()
{
    /*
     *  Call init cache 
     *  Send & recv echo to ensure cache service made the connect, which is async.
     * 
     *  Read events for 2 seconds in non-block mode to drain the local zmq cache
     *  maintained by zmq lib.
     *
     *  Disconnect the SUBS socket
     *  Send the read events to cache service star, as initial stock.
     */
    int rc = 0;

    if (m_event_service.is_active()) {
        event_serialized_lst_t events;

        rc = m_event_service.cache_init();
        RET_ON_ERR(rc == 0, "Failed to init the cache");

        /* Shadow the cache init request, as it is async */
        m_event_service.send_recv(EVENT_ECHO);

        /*
         * read for a second in non-block mode, to drain any local cache.
         * Break if no event locally available or 1 second passed, whichever
         * comes earlier.
         */
        chrono::steady_clock::time_point start = chrono::steady_clock::now();
        while(true) {
            string source;
            event_serialized_t evt_str;
            internal_event_t evt_data;

            rc = zmq_message_read(m_socket, ZMQ_DONTWAIT, source, evt_data);
            if (rc != 0) {
                /* Break on any failure, including EAGAIN */
                break;
            }

            serialize(evt_data, evt_str);
            events.push_back(evt_str);
            chrono::steady_clock::time_point now = chrono::steady_clock::now();
            if (chrono::duration_cast<chrono::milliseconds>(now - start).count() >
                    CACHE_DRAIN_IN_MILLISECS)
                break;
        }

        /* Start cache service with locally read events as initial stock */
        RET_ON_ERR(m_event_service.cache_start(events) == 0,
                "Failed to send cache start");
        m_event_service.close_service();
    }
out:
    if (m_socket != NULL) {
        zmq_close(m_socket);
    }
    zmq_ctx_term(m_zmq_ctx);
}


int
EventSubscriber::init(bool use_cache, int recv_timeout,
        const event_subscribe_sources_t *subs_sources)
{
    /*
     *  Initiate SUBS connection to XPUB end point.
     *  Send subscribe request
     *
     *  If cache use enabled, buy some time to shadow
     *  the earlier SUBS connect, which is async, before
     *  calling event_service to stop cache
     */
    m_zmq_ctx = zmq_ctx_new();

    void *sock = zmq_socket (m_zmq_ctx, ZMQ_SUB);

    int rc = zmq_connect (sock, get_config(XPUB_END_KEY).c_str());
    RET_ON_ERR(rc == 0, "Subscriber fails to connect %s", get_config(XPUB_END_KEY).c_str());

    if ((subs_sources == NULL) || (subs_sources->empty())) {
        rc = zmq_setsockopt(sock, ZMQ_SUBSCRIBE, "", 0);
        RET_ON_ERR(rc == 0, "Fails to set option");
    }
    else {
        for (const auto e: *subs_sources) {
            rc = zmq_setsockopt(sock, ZMQ_SUBSCRIBE, e.c_str(), e.size());
            RET_ON_ERR(rc == 0, "Fails to set option");
        }
    }

    if (recv_timeout != -1) {
        rc = zmq_setsockopt (sock, ZMQ_RCVTIMEO, &recv_timeout, sizeof (recv_timeout));
        RET_ON_ERR(rc == 0, "Failed to ZMQ_RCVTIMEO to %d", recv_timeout);
    }

    if (use_cache) {
        rc = m_event_service.init_client(m_zmq_ctx, EVENTS_SERVICE_TIMEOUT_MS_SUB);
        RET_ON_ERR(rc == 0, "Fails to init the service");

        if (m_event_service.cache_stop() == 0) {
            // Stopped an active cache 
            m_cache_read = true;
        }
    }
    m_socket = sock;
out:
    if (m_socket == NULL) {
        zmq_close(sock);
    }
    return rc;
}


void
EventSubscriber::prune_track()
{
    map<time_t, vector<runtime_id_t> > lst;

    /* Sort entries by last touched time */
    for(const auto e: m_track) {
        lst[e.second.epoch_secs].push_back(e.first);
    }

    /* By default it walks from lowest value / earliest timestamp */
    map<time_t, vector<runtime_id_t> >::const_iterator itc = lst.begin();
    for(; (itc != lst.end()) && (m_track.size() > MAX_PUBLISHERS_COUNT); ++itc) {
        for (const auto r: itc->second) {
            m_track.erase(r);
        }
    }
        
}


event_receive_op_t
EventSubscriber::event_receive()
{
    event_receive_op_t evt;
    int missed_cnt = 0;

    while (evt.event.empty()) {
        internal_event_t event_data;

        if (m_cache_read && m_from_cache.empty()) {
            m_event_service.cache_read(m_from_cache);
            m_cache_read = !m_from_cache.empty();
        }

        if (!m_from_cache.empty()) {

            event_serialized_lst_t::iterator it = m_from_cache.begin();
            rc = deserialize(*it, event_data);
            m_from_cache.erase(it);
            RET_ON_ERR(rc == 0, "Failed to deserialize message from cache");

        }
        else {
            /* Read from SUBS channel */
            string evt_source;
            rc = zmq_message_read(m_socket, 0, evt_source, event_data);
            RET_ON_ERR(rc == 0, "Failed to read message from sock rc=%d", rc);
        }

        /* Find any missed events for this runtime ID */
        int this_missed_cnt = 0;
        sequence_t seq = str_to_seq(event_data[EVENT_SEQUENCE]);
        track_info_t::const_iterator itc = m_track.find(event_data[EVENT_RUNTIME_ID]);
        if (itc != m_track.end()) {
            /* current seq - last read - 1 == 0 if none missed */
            if (seq != 0) {
                this_missed_cnt = seq - itc->second.seq - 1;
            }
            else {
                /* handle rollover */
                this_missed_cnt = SEQUENCE_MAX - itc->second.seq;
            }
        }
        else {
            /*
             * First message seen from this runtime id, implying
             * all earlier messages are lost.
             */
            if (seq != 0) {
                this_missed_cnt = seq - 1;
            }
            /* roll over is nearly impossible - 4GB events not seen */

            /* May need to add new ID to track; Prune if needed */
            if (m_track.size() > (MAX_PUBLISHERS_COUNT + 10)) {
                prune_track();
            }
        }
        missed_cnt += this_missed_cnt;

        if (event_data[EVENT_STR_DATA].compare(0, EVENT_STR_CTRL_PREFIX_SZ,
                    EVENT_STR_CTRL_PREFIX) != 0) {
            if (this_missed_cnt >= 0) {
                istringstream iss(event_data[EVENT_EPOCH]);

                evt.event = event_data[EVENT_STR_DATA];
                evt.publish_epoch << iss;

                m_track[event_data[EVENT_RUNTIME_ID]] = evt_info_t(seq);
            }
            else {
                /* negative value implies duplicate; Possibly seen from cache */
            }
        } else {
            /* This is control message */
            if (event_data[EVENT_STR_DATA] == EVENT_STR_CTRL_DEINIT) {
                if (itc != m_track.end()) {
                    m_track.erase(event_data[EVENT_RUNTIME_ID]);
                }
            }
        }
    }
out:
    evt.rc = rc;
    evt.missed_cnt = missed_cnt;
    return evt;
}

event_handle_t
events_init_subscriber(bool use_cache, int recv_timeout,
        const event_subscribe_sources_t *sources)
{
    return EventSubscriber::get_subscriber(use_cache, recv_timeout, sources);
}

void
events_deinit_subscriber(event_handle_t handle)
{
    EventSubscriber::drop_subscriber(handle);
}

event_receive_op_t
event_receive(event_handle_t handle)
{
    return EventSubscriber::do_receive(handle);
}

void *
events_init_publisher_wrap(const char *args)
{
    SWSS_LOG_DEBUG("events_init_publisher_wrap: args=%s",
            (args != NULL ? args : "<null pointer>"));

    if (args == NULL) {
        return NULL;
    }
    const auto &data = nlohmann::json::parse(args);

    string source;
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        if ((it.key() == ARGS_SOURCE) && (*it).is_string()) {
            source = it.value();
        }
    }
    return events_init_publisher(source);
}


void
events_deinit_publisher_wrap(void *handle) 
{
    events_deinit_publisher(handle);
}


int
event_publish_wrap(void *handle, const char *args)
{
    string tag;
    event_params_t params;

    SWSS_LOG_DEBUG("events_init_publisher_wrap: handle=%p args=%s",
            handle, (args != NULL ? args : "<null pointer>"));

    if (args == NULL) {
        return -1;
    }
    const auto &data = nlohmann::json::parse(args);

    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        if ((it.key() == ARGS_TAG) && (*it).is_string()) {
            tag = it.value();
        }
        else if ((it.key() == ARGS_PARAMS) && (*it).is_object()) {
            const auto &params_data = *it;
            for (auto itp = params_data.cbegin(); itp != params_data.cend(); ++itp) {
                if ((*itp).is_string()) {
                    params[itp.key()] = itp.value();
                }
            }
        }
    }
    return event_publish(handle, tag, &params);
}

void *
events_init_subscriber_wrap(const char *args)
{
    bool use_cache = true;
    int recv_timeout = -1;
    event_subscribe_sources_t sources;

    SWSS_LOG_DEBUG("events_init_subsriber_wrap: args:%s", args);

    if (args != NULL) {
        const auto &data = nlohmann::json::parse(args);

        for (auto it = data.cbegin(); it != data.cend(); ++it) {
            if ((it.key() == ARGS_USE_CACHE) && (*it).is_boolean()) {
                use_cache = it.value();
            }
            else if ((it.key() == ARGS_RECV_TIMEOUT) && (*it).is_number_integer()) {
                recv_timeout = it.value();
            }
        }
    }
    void *handle = events_init_subscriber(use_cache, recv_timeout);
    SWSS_LOG_DEBUG("events_init_subscriber_wrap: handle=%p", handle);
    return handle;
}


void
events_deinit_subscriber_wrap(void *handle) 
{
    SWSS_LOG_DEBUG("events_deinit_subsriber_wrap: args=%p", handle);

    events_deinit_subscriber(handle);
}


int
event_receive_wrap(void *handle, char *event_str, int event_str_sz)
{
    event_receive_op_t evt;
    int rc = 0;

    SWSS_LOG_DEBUG("events_receive_wrap handle=%p event-sz=%d missed-sz=%d\n",
            handle, event_str_sz, missed_cnt_str_sz);

    evt = event_receive(handle);

    if (evt.rc == 0) {

        string rcv_str(evt.to_json());
        strncpy(event_str, rcv_str.c_str(), event_str_sz);
        event_str[event_str-1] = 0;
        rc = rcv_str.size();
    }
    else if (evt.rc > 0) {
        // timeout
        rc = 0;
    }
    else {
        rc = evt.rc;
    }

    SWSS_LOG_DEBUG("events_receive_wrap rc=%d event_str=%s missed=%s\n",
            rc, event_str, missed_cnt_str);

    return rc;
}


void swssSetLogPriority(int pri)
{
    swss::Logger::setMinPrio((swss::Logger::Priority) pri);
}

string
event_receive_op_t::to_json()
{
    nlohmann::json res = nlohmann::json::object();

    res[RC_KEY] = rc;
    res[EVENT_KEY] = event;
    res[MISSED_KEY] = missed_cnt;
    res[EPOCH_KEY] = publish_epoch;

    return res.dump();
}


int
event_receive_op_t::parse_event(string &key, map_str_str_t &params)
{
    return convert_from_json(event, key, params);
}

