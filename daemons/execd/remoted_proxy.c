/*
 * Copyright 2012-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <glib.h>
#include <unistd.h>

#include "pacemaker-execd.h"
#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/services.h>
#include <crm/common/mainloop.h>
#include <crm/common/ipc.h>
#include <crm/common/ipcs.h>
#include <crm/cib/internal.h>
#include <crm/fencing/internal.h>

static qb_ipcs_service_t *cib_ro = NULL;
static qb_ipcs_service_t *cib_rw = NULL;
static qb_ipcs_service_t *cib_shm = NULL;

static qb_ipcs_service_t *attrd_ipcs = NULL;
static qb_ipcs_service_t *crmd_ipcs = NULL;
static qb_ipcs_service_t *stonith_ipcs = NULL;

// An IPC provider is a cluster node controller connecting as a client
static GList *ipc_providers = NULL;
/* ipc clients == things like cibadmin, crm_resource, connecting locally */
static GHashTable *ipc_clients = NULL;

/*!
 * \internal
 * \brief Get an IPC proxy provider
 *
 * \return Pointer to a provider if one exists, NULL otherwise
 *
 * \note Grab the first provider, which is the most recent connection. That way,
 *       if we haven't yet timed out an old, failed connection, we don't try to
 *       use it.
 */
crm_client_t *
ipc_proxy_get_provider()
{
    return ipc_providers? (crm_client_t*) (ipc_providers->data) : NULL;
}

/*!
 * \internal
 * \brief Accept a client connection on a proxy IPC server
 *
 * \param[in] c            Client's IPC connection
 * \param[in] uid          Client's user ID
 * \param[in] gid          Client's group ID
 * \param[in] ipc_channel  Name of IPC server to proxy
 *
 * \return pcmk_ok on success, -errno on error
 */
static int32_t
ipc_proxy_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid, const char *ipc_channel)
{
    crm_client_t *client;
    crm_client_t *ipc_proxy = ipc_proxy_get_provider();
    xmlNode *msg;

    if (ipc_proxy == NULL) {
        crm_warn("Cannot proxy IPC connection from uid %d gid %d to %s "
                 "because not connected to cluster", uid, gid, ipc_channel);
        return -EREMOTEIO;
    }

    /* This new client is a local IPC client on a Pacemaker Remote controlled
     * node, needing to access cluster node IPC services.
     */
    client = crm_client_new(c, uid, gid);
    if (client == NULL) {
        return -EREMOTEIO;
    }

    /* This ipc client is bound to a single ipc provider. If the
     * provider goes away, this client is disconnected */
    client->userdata = strdup(ipc_proxy->id);
    client->name = crm_strdup_printf("proxy-%s-%d-%.8s", ipc_channel, client->pid, client->id);

    g_hash_table_insert(ipc_clients, client->id, client);

    msg = create_xml_node(NULL, T_LRMD_IPC_PROXY);
    crm_xml_add(msg, F_LRMD_IPC_OP, LRMD_IPC_OP_NEW);
    crm_xml_add(msg, F_LRMD_IPC_IPC_SERVER, ipc_channel);
    crm_xml_add(msg, F_LRMD_IPC_SESSION, client->id);
    lrmd_server_send_notify(ipc_proxy, msg);
    free_xml(msg);
    crm_debug("Accepted IPC proxy connection (session ID %s) "
              "from uid %d gid %d on channel %s",
              client->id, uid, gid, ipc_channel);
    return 0;
}

static int32_t
crmd_proxy_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    return ipc_proxy_accept(c, uid, gid, CRM_SYSTEM_CRMD);
}

static int32_t
attrd_proxy_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    return ipc_proxy_accept(c, uid, gid, T_ATTRD);
}

static int32_t
stonith_proxy_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    return ipc_proxy_accept(c, uid, gid, "stonith-ng");
}

static int32_t
cib_proxy_accept_rw(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    return ipc_proxy_accept(c, uid, gid, CIB_CHANNEL_RW);
}

static int32_t
cib_proxy_accept_ro(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    return ipc_proxy_accept(c, uid, gid, CIB_CHANNEL_RO);
}

static void
ipc_proxy_created(qb_ipcs_connection_t * c)
{
    crm_trace("Connection %p", c);
}

void
ipc_proxy_forward_client(crm_client_t *ipc_proxy, xmlNode *xml)
{
    const char *session = crm_element_value(xml, F_LRMD_IPC_SESSION);
    const char *msg_type = crm_element_value(xml, F_LRMD_IPC_OP);
    xmlNode *msg = get_message_xml(xml, F_LRMD_IPC_MSG);
    crm_client_t *ipc_client;
    int rc = 0;

    /* If the IPC provider is acknowledging our shutdown request,
     * defuse the short exit timer to give the cluster time to
     * stop any resources we're running.
     */
    if (safe_str_eq(msg_type, LRMD_IPC_OP_SHUTDOWN_ACK)) {
        handle_shutdown_ack();
        return;
    }

    if (safe_str_eq(msg_type, LRMD_IPC_OP_SHUTDOWN_NACK)) {
        handle_shutdown_nack();
        return;
    }

    ipc_client = crm_client_get_by_id(session);
    if (ipc_client == NULL) {
        xmlNode *msg = create_xml_node(NULL, T_LRMD_IPC_PROXY);
        crm_xml_add(msg, F_LRMD_IPC_OP, LRMD_IPC_OP_DESTROY);
        crm_xml_add(msg, F_LRMD_IPC_SESSION, session);
        lrmd_server_send_notify(ipc_proxy, msg);
        free_xml(msg);
        return;
    }

    /* This is an event or response from the ipc provider
     * going to the local ipc client.
     *
     * Looking at the chain of events.
     *
     * -----remote node----------------|---- cluster node ------
     * ipc_client <--1--> this code
     *    <--2--> pacemaker-controld:remote_proxy_cb/remote_proxy_relay_event()
     *    <--3--> ipc server
     *
     * This function is receiving a msg from connection 2
     * and forwarding it to connection 1.
     */

    if (safe_str_eq(msg_type, LRMD_IPC_OP_EVENT)) {
        crm_trace("Sending event to %s", ipc_client->id);
        rc = crm_ipcs_send(ipc_client, 0, msg, crm_ipc_server_event);

    } else if (safe_str_eq(msg_type, LRMD_IPC_OP_RESPONSE)) {
        int msg_id = 0;

        crm_element_value_int(xml, F_LRMD_IPC_MSG_ID, &msg_id);
        crm_trace("Sending response to %d - %s", ipc_client->request_id, ipc_client->id);
        rc = crm_ipcs_send(ipc_client, msg_id, msg, FALSE);

        CRM_LOG_ASSERT(msg_id == ipc_client->request_id);
        ipc_client->request_id = 0;

    } else if (safe_str_eq(msg_type, LRMD_IPC_OP_DESTROY)) {
        qb_ipcs_disconnect(ipc_client->ipcs);

    } else {
        crm_err("Unknown ipc proxy msg type %s" , msg_type);
    }

    if (rc < 0) {
        crm_warn("IPC Proxy send to ipc client %s failed, rc = %d", ipc_client->id, rc);
    }
}

static int32_t
ipc_proxy_dispatch(qb_ipcs_connection_t * c, void *data, size_t size)
{
    uint32_t id = 0;
    uint32_t flags = 0;
    crm_client_t *client = crm_client_get(c);
    crm_client_t *ipc_proxy = crm_client_get_by_id(client->userdata);
    xmlNode *request = NULL;
    xmlNode *msg = NULL;

    if (!ipc_proxy) {
        qb_ipcs_disconnect(client->ipcs);
        return 0;
    }

    /* This is a request from the local ipc client going
     * to the ipc provider.
     *
     * Looking at the chain of events.
     *
     * -----remote node----------------|---- cluster node ------
     * ipc_client <--1--> this code
     *     <--2--> pacemaker-controld:remote_proxy_dispatch_internal()
     *     <--3--> ipc server
     *
     * This function is receiving a request from connection
     * 1 and forwarding it to connection 2.
     */
    request = crm_ipcs_recv(client, data, size, &id, &flags);

    if (!request) {
        return 0;
    }

    CRM_CHECK(client != NULL, crm_err("Invalid client");
              free_xml(request); return FALSE);
    CRM_CHECK(client->id != NULL, crm_err("Invalid client: %p", client);
              free_xml(request); return FALSE);

    /* This ensures that synced request/responses happen over the event channel
     * in the controller, allowing the controller to process the messages async.
     */
    set_bit(flags, crm_ipc_proxied);
    client->request_id = id;

    msg = create_xml_node(NULL, T_LRMD_IPC_PROXY);
    crm_xml_add(msg, F_LRMD_IPC_OP, LRMD_IPC_OP_REQUEST);
    crm_xml_add(msg, F_LRMD_IPC_SESSION, client->id);
    crm_xml_add(msg, F_LRMD_IPC_CLIENT, crm_client_name(client));
    crm_xml_add(msg, F_LRMD_IPC_USER, client->user);
    crm_xml_add_int(msg, F_LRMD_IPC_MSG_ID, id);
    crm_xml_add_int(msg, F_LRMD_IPC_MSG_FLAGS, flags);
    add_message_xml(msg, F_LRMD_IPC_MSG, request);
    lrmd_server_send_notify(ipc_proxy, msg);
    free_xml(request);
    free_xml(msg);

    return 0;
}

/*!
 * \internal
 * \brief Notify a proxy provider that we wish to shut down
 *
 * \return 0 on success, -1 on error
 */
int
ipc_proxy_shutdown_req(crm_client_t *ipc_proxy)
{
    xmlNode *msg = create_xml_node(NULL, T_LRMD_IPC_PROXY);
    int rc;

    crm_xml_add(msg, F_LRMD_IPC_OP, LRMD_IPC_OP_SHUTDOWN_REQ);

    /* We don't really have a session, but the controller needs this attribute
     * to recognize this as proxy communication.
     */
    crm_xml_add(msg, F_LRMD_IPC_SESSION, "0");

    rc = (lrmd_server_send_notify(ipc_proxy, msg) < 0)? -1 : 0;
    free_xml(msg);
    return rc;
}

static int32_t
ipc_proxy_closed(qb_ipcs_connection_t * c)
{
    crm_client_t *client = crm_client_get(c);
    crm_client_t *ipc_proxy;

    if (client == NULL) {
        return 0;
    }

    ipc_proxy = crm_client_get_by_id(client->userdata);

    crm_trace("Connection %p", c);

    if (ipc_proxy) {
        xmlNode *msg = create_xml_node(NULL, T_LRMD_IPC_PROXY);
        crm_xml_add(msg, F_LRMD_IPC_OP, LRMD_IPC_OP_DESTROY);
        crm_xml_add(msg, F_LRMD_IPC_SESSION, client->id);
        lrmd_server_send_notify(ipc_proxy, msg);
        free_xml(msg);
    }

    g_hash_table_remove(ipc_clients, client->id);

    free(client->userdata);
    client->userdata = NULL;
    crm_client_destroy(client);
    return 0;
}

static void
ipc_proxy_destroy(qb_ipcs_connection_t * c)
{
    crm_trace("Connection %p", c);
    ipc_proxy_closed(c);
}

static struct qb_ipcs_service_handlers crmd_proxy_callbacks = {
    .connection_accept = crmd_proxy_accept,
    .connection_created = ipc_proxy_created,
    .msg_process = ipc_proxy_dispatch,
    .connection_closed = ipc_proxy_closed,
    .connection_destroyed = ipc_proxy_destroy
};

static struct qb_ipcs_service_handlers attrd_proxy_callbacks = {
    .connection_accept = attrd_proxy_accept,
    .connection_created = ipc_proxy_created,
    .msg_process = ipc_proxy_dispatch,
    .connection_closed = ipc_proxy_closed,
    .connection_destroyed = ipc_proxy_destroy
};

static struct qb_ipcs_service_handlers stonith_proxy_callbacks = {
    .connection_accept = stonith_proxy_accept,
    .connection_created = ipc_proxy_created,
    .msg_process = ipc_proxy_dispatch,
    .connection_closed = ipc_proxy_closed,
    .connection_destroyed = ipc_proxy_destroy
};

static struct qb_ipcs_service_handlers cib_proxy_callbacks_ro = {
    .connection_accept = cib_proxy_accept_ro,
    .connection_created = ipc_proxy_created,
    .msg_process = ipc_proxy_dispatch,
    .connection_closed = ipc_proxy_closed,
    .connection_destroyed = ipc_proxy_destroy
};

static struct qb_ipcs_service_handlers cib_proxy_callbacks_rw = {
    .connection_accept = cib_proxy_accept_rw,
    .connection_created = ipc_proxy_created,
    .msg_process = ipc_proxy_dispatch,
    .connection_closed = ipc_proxy_closed,
    .connection_destroyed = ipc_proxy_destroy
};

void
ipc_proxy_add_provider(crm_client_t *ipc_proxy)
{
    // Prepending ensures the most recent connection is always first
    ipc_providers = g_list_prepend(ipc_providers, ipc_proxy);
}

void
ipc_proxy_remove_provider(crm_client_t *ipc_proxy)
{
    GHashTableIter iter;
    crm_client_t *ipc_client = NULL;
    char *key = NULL;
    GList *remove_these = NULL;
    GListPtr gIter = NULL;

    ipc_providers = g_list_remove(ipc_providers, ipc_proxy);

    g_hash_table_iter_init(&iter, ipc_clients);
    while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & ipc_client)) {
        const char *proxy_id = ipc_client->userdata;
        if (safe_str_eq(proxy_id, ipc_proxy->id)) {
            crm_info("ipc proxy connection for client %s pid %d destroyed because cluster node disconnected.",
                ipc_client->id, ipc_client->pid);
            /* we can't remove during the iteration, so copy items
             * to a list we can destroy later */
            remove_these = g_list_append(remove_these, ipc_client);
        }
    }

    for (gIter = remove_these; gIter != NULL; gIter = gIter->next) {
        ipc_client = gIter->data;

        // Disconnection callback will free the client here
        qb_ipcs_disconnect(ipc_client->ipcs);
    }

    /* just frees the list, not the elements in the list */
    g_list_free(remove_these);
}

void
ipc_proxy_init(void)
{
    ipc_clients = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, NULL);

    cib_ipc_servers_init(&cib_ro,
                         &cib_rw,
                         &cib_shm,
                         &cib_proxy_callbacks_ro,
                         &cib_proxy_callbacks_rw);

    attrd_ipc_server_init(&attrd_ipcs, &attrd_proxy_callbacks);
    stonith_ipc_server_init(&stonith_ipcs, &stonith_proxy_callbacks);
    crmd_ipcs = crmd_ipc_server_init(&crmd_proxy_callbacks);
    if (crmd_ipcs == NULL) {
        crm_err("Failed to create controller: exiting and inhibiting respawn");
        crm_warn("Verify pacemaker and pacemaker_remote are not both enabled");
        crm_exit(CRM_EX_FATAL);
    }
}

void
ipc_proxy_cleanup(void)
{
    if (ipc_providers) {
        g_list_free(ipc_providers);
        ipc_providers = NULL;
    }
    if (ipc_clients) {
        g_hash_table_destroy(ipc_clients);
        ipc_clients = NULL;
    }
    cib_ipc_servers_destroy(cib_ro, cib_rw, cib_shm);
    qb_ipcs_destroy(attrd_ipcs);
    qb_ipcs_destroy(stonith_ipcs);
    qb_ipcs_destroy(crmd_ipcs);
    cib_ro = NULL;
    cib_rw = NULL;
    cib_shm = NULL;
}
