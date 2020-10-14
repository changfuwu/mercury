#include <assert.h>
#include <err.h>
#include <inttypes.h>   /* for PRIx8, etc. */
#include <libgen.h>     /* basename */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <sys/param.h>  /* for MIN, MAX */

#include <ucp/api/ucp.h>

#include "rxpool.h"
#include "util.h"
#include "wireup.h"

#define START_WIREUP_TAG 17

static void
usage(const char *_progname)
{
    char *progname = strdup(_progname);
    assert(progname != NULL);
    fprintf(stderr, "usage: %s [remote address]\n", basename(progname));
    free(progname);
    exit(EXIT_FAILURE);
}

static void
send_callback(void *request, ucs_status_t status, void *user_data)
{
    txdesc_t *desc = user_data;

    desc->status = status;

    desc->completed = true;
    ucp_request_free(request);
}

static void
ep_close(ucp_worker_h worker, ucp_ep_h ep)
{
    void *request;
    request = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FLUSH);
    if (request == UCS_OK)
        return;
    if (UCS_PTR_IS_ERR(request)) {
        warnx("%s: ucp_ep_close_nb: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
        return;
    }
    while (!ucp_request_is_completed(request))
        ucp_worker_progress(worker);
}

static void
run_client(ucp_worker_h worker, size_t request_size,
    ucp_address_t *local_addr, size_t local_addr_len,
    ucp_address_t *remote_addr, size_t remote_addr_len)
{
    rxpool_t rxpool;
    rxdesc_t *rdesc;
    ucs_status_t status;
    void *request;
    ucp_ep_h remote_ep;
    txdesc_t tdesc = {.completed = false};
    wireup_msg_t *reply, *req;
    size_t reqlen;
    const ucp_request_param_t send_params = {
      .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                      UCP_OP_ATTR_FIELD_USER_DATA 
    , .cb = {.send = send_callback}
    , .user_data = &tdesc
    };
    const ucp_ep_params_t ep_params = {
      .field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                    UCP_EP_PARAM_FIELD_ERR_HANDLER
    , .address = remote_addr
    , .err_mode = UCP_ERR_HANDLING_MODE_NONE
    };

    reqlen = sizeof(*req) + local_addr_len;
    if ((req = calloc(1, reqlen)) == NULL)
        err(EXIT_FAILURE, "%s: malloc", __func__);

    if ((status = ucp_ep_create(worker, &ep_params, &remote_ep)) != UCS_OK)
        errx(EXIT_FAILURE, "client %s: ucp_ep_create", __func__);

    rxpool_init(worker, &rxpool, request_size,
        START_WIREUP_TAG, UINT64_MAX, sizeof(wireup_msg_t), 3);

    req->op = OP_REQ;
    req->sender_id = 0;
    req->addrlen = local_addr_len;
    memcpy(&req->addr[0], local_addr, local_addr_len);

    request = ucp_tag_send_nbx(remote_ep, req, reqlen,
        START_WIREUP_TAG, &send_params);

    if (UCS_PTR_IS_ERR(request)) {
        warnx("%s: ucp_tag_send_nbx: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
    } else if (UCS_PTR_IS_PTR(request)) {
        while (!tdesc.completed)
            ucp_worker_progress(worker);
        if (tdesc.status != UCS_OK) {
            printf("send error, %s, exiting.\n",
                ucs_status_string(tdesc.status));
        }
        printf("send succeeded, exiting.\n");
    } else if (request == UCS_OK)
        printf("send succeeded immediately, exiting.\n");

    free(req);

    while ((rdesc = rxpool_next(&rxpool)) == NULL)
        ucp_worker_progress(worker);

    reply = rdesc->buf;

    assert(reply->op == OP_ACK);

    rxpool_destroy(&rxpool);

    ep_close(worker, remote_ep);
}

static void
process_rx_msg(ucp_worker_h worker, ucp_tag_t tag, void *buf, size_t buflen)
{
    ucp_ep_params_t ep_params = {
      .field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                    UCP_EP_PARAM_FIELD_ERR_HANDLER
    , .address = NULL
    , .err_mode = UCP_ERR_HANDLING_MODE_NONE
    };
    wireup_msg_t reply = (wireup_msg_t){ .sender_id = 0, .op = OP_ACK, .addrlen = 0};
    txdesc_t desc = {.completed = false};
    const ucp_request_param_t send_params = {
      .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                      UCP_OP_ATTR_FIELD_USER_DATA 
    , .cb = {.send = send_callback}
    , .user_data = &desc
    };
    void *request;
    wireup_msg_t *msg;
    ucp_ep_h reply_ep;
    const size_t hdrlen = offsetof(wireup_msg_t, addr[0]);
    ucs_status_t status;

    if (buflen < hdrlen) {
        warnx("%s: dropping %zu-byte message, shorter than header\n", __func__,
            buflen);
        return;
    }

    msg = buf;

    if (msg->op != OP_REQ) {
        warnx("%s: received unexpected %s-type op\n", __func__,
            wireup_op_string(msg->op));
        return;
    }

    if (buflen < offsetof(wireup_msg_t, addr[0]) + msg->addrlen) {
        warnx("%s: dropping %zu-byte message, address truncated\n",
            __func__, buflen);
    }
    ep_params.address = (void *)msg->addr;
    status = ucp_ep_create(worker, &ep_params, &reply_ep);
    /* TBD send nack on error */
    if (status != UCS_OK)
        warnx("%s: ucp_ep_create failed", __func__);

    request = ucp_tag_send_nbx(reply_ep, &reply, sizeof(reply),
        tag, &send_params);

    if (UCS_PTR_IS_ERR(request)) {
        warnx("%s: ucp_tag_send_nbx: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
    } else if (UCS_PTR_IS_PTR(request)) {
        while (!desc.completed)
            ucp_worker_progress(worker);
        if (desc.status != UCS_OK) {
            printf("send error, %s, exiting.\n",
                ucs_status_string(desc.status));
        }
        printf("send succeeded, exiting.\n");
    } else if (request == UCS_OK)
        printf("send succeeded immediately, exiting.\n");

    ep_close(worker, reply_ep);
}

static bool
run_server_once(rxpool_t *rxpool)
{
    rxdesc_t *rdesc;

    if ((rdesc = rxpool_next(rxpool)) == NULL)
        return true;

    if (rdesc->status == UCS_OK) {
        printf("received %zu-byte message tagged %" PRIu64
               ", processing...\n", rdesc->rxlen, rdesc->sender_tag);
        process_rx_msg(rxpool->worker, rdesc->sender_tag, rdesc->buf,
            rdesc->rxlen);
    } else if (rdesc->status == UCS_ERR_MESSAGE_TRUNCATED) {
        const size_t hdrlen = offsetof(wireup_msg_t, addr[0]);
        printf("%s: truncated desc %p buf %p buflen %zu\n", __func__,
           (void *)rdesc, rdesc->buf, rdesc->buflen);
        size_t buflen = rdesc->buflen;
        void * const buf = rdesc->buf, *nbuf;
        /* Twice the message length is twice the header length plus
         * twice the payload length, so subtract one header length.
         */
        size_t nbuflen = twice_or_max(buflen) - hdrlen;

        printf("increasing buffer length %zu -> %zu bytes.\n",
            buflen, nbuflen);

        if ((nbuf = malloc(nbuflen)) == NULL)
            err(EXIT_FAILURE, "%s: malloc", __func__);

        rdesc->buflen = nbuflen;
        rdesc->buf = nbuf;
        free(buf);
    } else {
        printf("receive error, %s, exiting.\n",
            ucs_status_string(rdesc->status));
        return false;
    }
    rxdesc_setup(rxpool, rdesc->buf, rdesc->buflen, rdesc);
    return true;
}

static void
run_server(ucp_worker_h worker, size_t request_size)
{
    rxpool_t rxpool;

    rxpool_init(worker, &rxpool, request_size, START_WIREUP_TAG, UINT64_MAX,
        sizeof(wireup_msg_t) + 93, 3);

    while (run_server_once(&rxpool))
            ucp_worker_progress(worker);

    rxpool_destroy(&rxpool);
}

int
main(int argc, char **argv)
{
    ucs_status_t status;
    ucp_config_t *config;
    ucp_context_h context;
    ucp_worker_h worker;
    ucp_address_t *local_addr;
    ucp_address_t *remote_addr;
    size_t i, local_addr_len, remote_addr_len;
    const char *delim = "";
    ucp_context_attr_t context_attrs;
    ucp_params_t global_params = {
      .field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_REQUEST_SIZE |
                    UCP_PARAM_FIELD_REQUEST_INIT
    , .features = UCP_FEATURE_TAG | UCP_FEATURE_RMA
    , .request_size = sizeof(rxdesc_t)
    , .request_init = rxdesc_init
    };
    ucp_worker_params_t worker_params = {
      .field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE
    , .thread_mode = UCS_THREAD_MODE_MULTI
    };

    if (argc > 2)
        usage(argv[0]);
    if (argc == 2) {
        uint8_t *buf;

        if (colon_separated_octets_to_bytes(argv[1], &buf,
                                            &remote_addr_len) == -1)
            errx(EXIT_FAILURE, "could not parse remote address `%s`", argv[1]);
        printf("parsed %zu-byte remote address\n", remote_addr_len);
        remote_addr = (void *)buf;
    } else {
        remote_addr = NULL;
    }

    if ((status = ucp_config_read(NULL, NULL, &config)) != UCS_OK)
        errx(EXIT_FAILURE, "%s: ucp_config_read", __func__);

    status = ucp_init(&global_params, config, &context);

    ucp_config_release(config);

    if (status != UCS_OK)
        errx(EXIT_FAILURE, "%s: ucp_init", __func__);

    context_attrs.field_mask = UCP_ATTR_FIELD_REQUEST_SIZE;
    status = ucp_context_query(context, &context_attrs);

    if (status != UCS_OK)
        errx(EXIT_FAILURE, "%s: ucp_context_query", __func__);

    if ((context_attrs.field_mask & UCP_ATTR_FIELD_REQUEST_SIZE) == 0)
        errx(EXIT_FAILURE, "context attributes contain no request size");

    status = ucp_worker_create(context, &worker_params, &worker);
    if (status != UCS_OK) {
        warnx("%s: ucp_worker_create", __func__);
        goto cleanup_context;
    }

    status = ucp_worker_get_address(worker, &local_addr, &local_addr_len);
    if (status != UCS_OK) {
        warnx("%s: ucp_worker_get_address", __func__);
        goto cleanup_worker;
    }

    printf("%zu-byte local address ", local_addr_len);
    for (i = 0; i < local_addr_len; i++) {
        printf("%s%02" PRIx8, delim, ((uint8_t *)local_addr)[i]);
        delim = ":";
    }
    printf("\n");

    if (remote_addr != NULL) {      /* * * client mode * * */
        run_client(worker, context_attrs.request_size,
            local_addr, local_addr_len, remote_addr, remote_addr_len);
        free(remote_addr);
    } else {                        /* * * server mode * * */
        run_server(worker, context_attrs.request_size);
    }

    ucp_worker_release_address(worker, local_addr);
cleanup_worker:
    ucp_worker_destroy(worker);
cleanup_context:
    ucp_cleanup(context);
    return EXIT_SUCCESS;
}
