#include "common/logger.h"
#include "common/netlink.h"
#include "common/netdispatcher.h"

#include <errno.h>

#include <system_error>
#include <cstring>

using namespace swss;
using namespace std;

NetLink::NetLink(int pri) :
    Selectable(pri), m_socket(NULL)
{
    m_socket = nl_socket_alloc();
    if (!m_socket)
    {
        SWSS_LOG_ERROR("Unable to allocated netlink socket");
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to allocated netlink socket");
    }

    nl_socket_disable_seq_check(m_socket);
    nl_socket_modify_cb(m_socket, NL_CB_VALID, NL_CB_CUSTOM, onNetlinkMsg, this);

    int err = nl_connect(m_socket, NETLINK_ROUTE);
    if (err < 0)
    {
        SWSS_LOG_ERROR("Unable to connect netlink socket: %s", nl_geterror(err));
        nl_socket_free(m_socket);
        m_socket = NULL;
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect netlink socket");
    }

    nl_socket_set_nonblocking(m_socket);
    /* Set socket buffer size to 3MB */
    nl_socket_set_buffer_size(m_socket, 3145728, 0);
}

NetLink::~NetLink()
{
    if (m_socket != NULL)
    {
        nl_close(m_socket);
        nl_socket_free(m_socket);
    }
}

void NetLink::setRxBufSizeIfSupported(int size)
{
    int fd, err;
    char *increasedBufSupport;

    if (size < 0) {
        SWSS_LOG_ERROR("%s(): Rx Buffer size cannot be negative", __FUNCTION__);
    } else if (size <= 3145728) {
        /* Permissible buffer size for all systems */
        nl_socket_set_buffer_size(m_socket, 3145728, 0);
    } else {
        /* Check if the system is capable of the buffer size before setting it */
        increasedBufSupport = getenv("supports_increased_netlink_buffer");
        if (increasedBufSupport && !strcmp(increasedBufSupport, "true")) {
            fd =  nl_socket_get_fd(m_socket);
            if (fd <= 0) {
                SWSS_LOG_ERROR("%s(): Invlaid socket fd", __FUNCTION__);
                return;
            }
            /* We maybe going above the system default rmem, so use SO_RCVBUFFORCE */
            SWSS_LOG_INFO("%s(): Setting buffer size as %d for netlink fd = %d",
                    __FUNCTION__, size,  fd);
            err = setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size));
            if (err != 0) {
                SWSS_LOG_ERROR("%s(): Failed to set  buffer size as %d for netlink fd = %d",
                       __FUNCTION__, size, fd);
            }
        } else {
            SWSS_LOG_INFO("%s(): increasedBufSupport is %s, not applying the buffer size",
                    __FUNCTION__, increasedBufSupport);
        }
    }
}

void NetLink::registerGroup(int rtnlGroup)
{
    int err = nl_socket_add_membership(m_socket, rtnlGroup);
    if (err < 0)
    {
        SWSS_LOG_ERROR("Unable to register to group %d: %s", rtnlGroup,
                       nl_geterror(err));
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to register group");
    }
}

void NetLink::dumpRequest(int rtmGetCommand)
{
    int err = nl_rtgen_request(m_socket, rtmGetCommand, AF_UNSPEC, NLM_F_DUMP);
    if (err < 0)
    {
        SWSS_LOG_ERROR("Unable to request dump on group %d: %s", rtmGetCommand,
                       nl_geterror(err));
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to request dump");
    }
}

int NetLink::getFd()
{
    return nl_socket_get_fd(m_socket);
}

uint64_t NetLink::readData()
{
    int err;

    do
    {
        err = nl_recvmsgs_default(m_socket);
    }
    while(err == -NLE_INTR); // Retry if the process was interrupted by a signal

    if (err < 0)
    {
        if (err == -NLE_NOMEM)
            SWSS_LOG_ERROR("netlink reports out of memory on reading a netlink socket. High possibility of a lost message");
        else if (err == -NLE_AGAIN)
            SWSS_LOG_DEBUG("netlink reports NLE_AGAIN on reading a netlink socket");
        else
            SWSS_LOG_ERROR("netlink reports an error=%d on reading a netlink socket", err);
    }
    return 0;
}

int NetLink::onNetlinkMsg(struct nl_msg *msg, void *arg)
{
    NetDispatcher::getInstance().onNetlinkMessage(msg);
    return NL_OK;
}
