/*
 * function_shipper_handler.c
 */

#include "function_shipper_handler.h"
#include "function_shipper.h"
#include "function_map.h"
#include "iofsl_compat.h"
#include "shipper_error.h"

typedef struct fs_proc_info {
    int (*fs_routine) (fs_handle_t handle);
    int (*dec_routine)(fs_proc_t proc, void *in_struct);
    int (*enc_routine)(fs_proc_t proc, void *out_struct);
} fs_proc_info_t;

typedef struct fs_priv_handle {
    fs_id_t   id;
    na_addr_t addr;
    na_tag_t  tag;
    fs_proc_t dec_proc;
    void     *recv_buf;
    void     *in_struct;
} fs_priv_handle_t;

/* Function map */
static func_map_t *handler_func_map;

/* Network class */
static na_network_class_t *handler_network_class = NULL;


/*---------------------------------------------------------------------------
 * Function:    fs_handler_init
 *
 * Purpose:     Initialize the function shipper and select a network protocol
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
int fs_handler_init(na_network_class_t *network_class)
{
    int ret = S_SUCCESS;

    if (handler_network_class) {
        S_ERROR_DEFAULT("Already initialized");
        ret = S_FAIL;
        return ret;
    }

    handler_network_class = network_class;

    /* Create new function map */
    handler_func_map = func_map_new();

    return ret;
}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_finalize
 *
 * Purpose:     Finalize the function shipper
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
int fs_handler_finalize(void)
{
    int ret = S_SUCCESS;

    if (!handler_network_class) {
        S_ERROR_DEFAULT("Already finalized");
        ret = S_FAIL;
        return ret;
    }

    na_finalize(handler_network_class);

    /* Delete function map */
    func_map_free(handler_func_map);
    handler_func_map = NULL;

    handler_network_class = NULL;

    return ret;
}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_register
 *
 * Purpose:     Register a function name and provide a unique ID
 *
 * Returns:     Unsigned integer
 *
 *---------------------------------------------------------------------------
 */
void fs_handler_register(const char *func_name,
        int (*fs_routine) (fs_handle_t handle),
        int (*dec_routine)(fs_proc_t proc, void *in_struct),
        int (*enc_routine)(fs_proc_t proc, void *out_struct))
{
    fs_id_t *id;
    fs_proc_info_t *proc_info;

    /* Generate a key from the string */
    id = malloc(sizeof(fs_id_t));

    *id = fs_proc_string_hash(func_name);

    /* Fill a func info struct and store it into the function map */
    proc_info = malloc(sizeof(fs_proc_info_t));

    proc_info->fs_routine  = fs_routine;
    proc_info->dec_routine = dec_routine;
    proc_info->enc_routine = enc_routine;
    func_map_insert(handler_func_map, id, proc_info);
}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_get_input
 *
 * Purpose:     Get input from handle
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
int fs_handler_get_input(fs_handle_t handle, void *in_struct)
{
    fs_priv_handle_t *priv_handle = (fs_priv_handle_t *) handle;
    fs_proc_info_t   *proc_info;
    int ret = S_SUCCESS;

    if (priv_handle->recv_buf && priv_handle->dec_proc) {

        /* Retrieve decoding function from function map */
        proc_info = func_map_lookup(handler_func_map, &priv_handle->id);
        if (!proc_info) {
            S_ERROR_DEFAULT("func_map_lookup failed");
            ret = S_FAIL;
            return ret;
        }

        /* Decode input parameters */
        proc_info->dec_routine(priv_handle->dec_proc, in_struct);

        /* Free the decoding proc */
        fs_proc_free(priv_handle->dec_proc);
        priv_handle->dec_proc = NULL;

        /* Free recv buf */
        free(priv_handle->recv_buf);
        priv_handle->recv_buf = NULL;
    }

    return ret;
}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_get_addr
 *
 * Purpose:     Get remote addr from handle
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
const na_addr_t fs_handler_get_addr (fs_handle_t handle)
{
    fs_priv_handle_t *priv_handle = (fs_priv_handle_t *) handle;
    na_addr_t ret = NULL;

    if (priv_handle) ret = priv_handle->addr;

    return ret;
}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_receive
 *
 * Purpose:     Receive a new function call
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
int fs_handler_receive(void)
{
    na_size_t         recv_buf_len = 0;
    fs_priv_handle_t *priv_handle = NULL;
    fs_proc_info_t   *proc_info;

    void      *dec_buf_ptr;
    na_size_t  dec_buf_len = 0;

    int ret = S_SUCCESS;

    /* Create a new handle */
    priv_handle = malloc(sizeof(fs_priv_handle_t));
    priv_handle->id = 0;
    priv_handle->addr = NULL;
    priv_handle->tag = 0;
    priv_handle->dec_proc = NULL;
    priv_handle->recv_buf = NULL;
    priv_handle->in_struct = NULL;

    /* Do not expect message bigger than unexpected size (otherwise something went wrong) */
    recv_buf_len = na_get_unexpected_size(handler_network_class);
    priv_handle->recv_buf = malloc(recv_buf_len);

    /* Recv a message from a client (blocking for now) */
    na_recv_unexpected(handler_network_class, priv_handle->recv_buf, &recv_buf_len,
            &priv_handle->addr, &priv_handle->tag, NULL, NULL);

    dec_buf_ptr = priv_handle->recv_buf;
    dec_buf_len = recv_buf_len;

    /* Decode IOFSL id (used for compat) */
    iofsl_compat_proc_dec_id(dec_buf_ptr, dec_buf_len);
    dec_buf_ptr += iofsl_compat_get_size_id();
    dec_buf_len -= iofsl_compat_get_size_id();

    /* Create a new decoding proc */
    fs_proc_create(dec_buf_ptr, dec_buf_len, FS_DECODE, &priv_handle->dec_proc);

    /* Get generic op id */
    fs_proc_uint32_t(priv_handle->dec_proc, &priv_handle->id);

    /* Retrieve exe function from function map */
    proc_info = func_map_lookup(handler_func_map, &priv_handle->id);
    if (!proc_info) {
        S_ERROR_DEFAULT("func_map_lookup failed");
        ret = S_FAIL;
        return ret;
    }

    /* Execute function and fill output parameters */
    proc_info->fs_routine(priv_handle);

    return ret;
}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_execute
 *
 * Purpose:     Execute the function call
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
//int fs_handler_execute(fs_handle_t handle)
//{
//    fs_priv_handle_t *priv_handle = (fs_priv_handle_t *) handle;
//    int ret = S_SUCCESS;
//
//
//
//    return ret;
//}

/*---------------------------------------------------------------------------
 * Function:    fs_handler_complete
 *
 * Purpose:     Send the response back to the caller
 *
 * Returns:     Non-negative on success or negative on failure
 *
 *---------------------------------------------------------------------------
 */
int fs_handler_complete(fs_handle_t handle, const void *out_struct)
{
    void *send_buf = NULL;

    /* buf len is the size of an unexpected message by default */
    na_size_t send_buf_len = na_get_unexpected_size(handler_network_class);
    /* Send buf len may be determined once the encoding function is called */
//    na_size_t send_buf_len = 0;
//    na_size_t min_send_buf_len = 0;

    fs_proc_t  enc_proc;
    void      *enc_buf_ptr;
    na_size_t  enc_buf_len = 0;

    na_request_t send_request = NULL;

    fs_proc_info_t   *proc_info;
    fs_priv_handle_t *priv_handle = (fs_priv_handle_t *) handle;

    int ret = S_SUCCESS;

    /* Retrieve encoding function from function map */
    proc_info = func_map_lookup(handler_func_map, &priv_handle->id);
    if (!proc_info) {
        S_ERROR_DEFAULT("func_map_lookup failed");
        ret = S_FAIL;
        return ret;
    }

    /* Get the minimum encoding size */
//    proc_info->enc_routine(NULL, &min_send_buf_len, NULL);
//    if (min_send_buf_len == 0) {
//        S_ERROR_DEFAULT("encoding function requires a non-zero buffer length");
//        ret = S_FAIL;
//        return ret;
//    }
    /* We need some extra space to add IOFSL stuff */
//    min_send_buf_len += iofsl_compat_get_size_status();

//    if (min_send_buf_len < na_get_unexpected_size(handler_network_class)) {
//        send_buf_len = na_get_unexpected_size(handler_network_class);
//    } else {
//        S_ERROR_DEFAULT("Buffer length currently not supported");
//        ret = S_FAIL;
//        return ret;
//    }

    send_buf = malloc(send_buf_len);
    if (!send_buf) {
        S_ERROR_DEFAULT("send buffer allocation failed.\n");
        ret = S_FAIL;
        return ret;
    }

    enc_buf_ptr = send_buf;
    enc_buf_len = send_buf_len;

    /* Simulate IOFSL behavior and add op status */
    iofsl_compat_proc_enc_status(enc_buf_ptr, enc_buf_len);
    enc_buf_ptr += iofsl_compat_get_size_status();
    enc_buf_len -= iofsl_compat_get_size_status();

    /* Create a new encoding proc */
    fs_proc_create(enc_buf_ptr, enc_buf_len, FS_ENCODE, &enc_proc);

    /* Encode output parameters */
    proc_info->enc_routine(enc_proc, (void*)out_struct);

    /* Respond back */
    na_send(handler_network_class, send_buf, send_buf_len, priv_handle->addr, priv_handle->tag, &send_request, NULL);

    na_wait(handler_network_class, send_request, NA_MAX_IDLE_TIME, NA_STATUS_IGNORE);

    /* Free the encoding proc */
    fs_proc_free(enc_proc);
    enc_proc = NULL;

    /* Free send buf */
    free(send_buf);
    send_buf = NULL;

    /* Free addr */
    na_addr_free(handler_network_class, priv_handle->addr);
    priv_handle->addr = NULL;

    /* Free in_struct */
    free(priv_handle->in_struct);
    priv_handle->in_struct = NULL;

    free(priv_handle);
    priv_handle = NULL;

    return ret;
}