/* zmq.c - wrapper functions for zmq prototyping */

#define _GNU_SOURCE
#include <stdio.h>
#include <zmq.h>
#include <czmq.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <json/json.h>
#include <assert.h>

#include "zmq.h"
#include "util.h"
#include "log.h"
#include "cmb.h"

#ifndef ZMQ_DONTWAIT
#   define ZMQ_DONTWAIT   ZMQ_NOBLOCK
#endif
#ifndef ZMQ_RCVHWM
#   define ZMQ_RCVHWM     ZMQ_HWM
#endif
#ifndef ZMQ_SNDHWM
#   define ZMQ_SNDHWM     ZMQ_HWM
#endif
#if ZMQ_VERSION_MAJOR == 2
#   define more_t int64_t
#   define zmq_ctx_destroy(context) zmq_term(context)
#   define zmq_msg_send(msg,sock,opt) zmq_send (sock, msg, opt)
#   define zmq_msg_recv(msg,sock,opt) zmq_recv (sock, msg, opt)
#   define ZMQ_POLL_MSEC    1000        //  zmq_poll is usec
#elif ZMQ_VERSION_MAJOR == 3
#   define more_t int
#   define ZMQ_POLL_MSEC    1           //  zmq_poll is msec
#endif

/**
 ** zmq wrappers
 **/

int zpoll (zmq_pollitem_t *items, int nitems, long timeout)
{
    int rc;

    if ((rc = zmq_poll (items, nitems, timeout * ZMQ_POLL_MSEC)) < 0)
        err_exit ("zmq_poll");
    return rc;
}

void zconnect (zctx_t *zctx, void **sp, int type, char *uri, int hwm)
{
    *sp = zsocket_new (zctx, type);
    zsocket_set_hwm (*sp, hwm);
    if (zsocket_connect (*sp, "%s", uri) < 0)
        err_exit ("zsocket_connect: %s", uri);
}

void zbind (zctx_t *zctx, void **sp, int type, char *uri, int hwm)
{
    *sp = zsocket_new (zctx, type);
    zsocket_set_hwm (*sp, hwm);
    if (zsocket_bind (*sp, "%s", uri) < 0)
        err_exit ("zsocket_bind: %s", uri);
}

zmsg_t *zmsg_recv_fd (int fd, int flags)
{
    char *buf;
    int n;
    zmsg_t *msg;

    buf = xzmalloc (CMB_API_BUFSIZE);
    n = recv (fd, buf, CMB_API_BUFSIZE, flags);
    if (n < 0)
        goto error;
    if (n == 0) {
        errno = EPROTO;
        goto error;
    }
    msg = zmsg_decode ((byte *)buf, n);
    free (buf);
    return msg;
error:
    free (buf);
    return NULL;
}

int zmsg_send_fd (int fd, zmsg_t **msg)
{
    char *buf = NULL;
    int len;

    len = zmsg_encode (*msg, (byte **)&buf);
    if (len < 0) {
        errno = EPROTO;
        goto error;
    }
    if (send (fd, buf, len, 0) < len)
        goto error;
    free (buf);
    zmsg_destroy (msg);
    return 0;
error:
    if (buf)
        free (buf);
    return -1;
}


/**
 ** cmb messages
 **/

static zframe_t *_tag_frame (zmsg_t *zmsg)
{
    zframe_t *zf;

    zf = zmsg_first (zmsg);
    while (zf && zframe_size (zf) != 0)
        zf = zmsg_next (zmsg); /* skip non-empty */
    if (zf)
        zf = zmsg_next (zmsg); /* skip empty */
    if (!zf)
        zf = zmsg_first (zmsg); /* rewind - there was no envelope */
    return zf;
}

static zframe_t *_json_frame (zmsg_t *zmsg)
{
    zframe_t *zf = _tag_frame (zmsg);

    return (zf ? zmsg_next (zmsg) : NULL);
}

static zframe_t *_data_frame (zmsg_t *zmsg)
{
    zframe_t *zf = _json_frame (zmsg);

    return (zf ? zmsg_next (zmsg) : NULL);
}

static zframe_t *_sender_frame (zmsg_t *zmsg)
{
    zframe_t *zf, *prev;

    zf = zmsg_first (zmsg);
    while (zf && zframe_size (zf) != 0) {
        prev = zf;
        zf = zmsg_next (zmsg);
    }
    return (zf ? prev : NULL);
}

int cmb_msg_decode (zmsg_t *zmsg, char **tagp, json_object **op,
                    void **datap, int *lenp)
{
    zframe_t *tag = _tag_frame (zmsg);
    zframe_t *json = zmsg_next (zmsg);
    zframe_t *data = zmsg_next (zmsg);

    if (!tag)
        goto eproto;
    if (tagp)
        *tagp = zframe_strdup (tag);
    if (op) {
        char *tmp = json ? zframe_strdup (json) : NULL;

        if (tmp) {
            *op = json_tokener_parse (tmp);
            free (tmp);
        } else
            *op = NULL;
    }
    if (datap && lenp) {
        if (data) {
            *lenp = zframe_size (data);
            *datap = xzmalloc (zframe_size (data));
            memcpy (*datap, zframe_data (data), zframe_size (data));
        } else {
            *lenp = 0;
            *datap = NULL;
        }
    }
    return 0;
eproto:
    errno = EPROTO;
    return -1;
}

int cmb_msg_recv (void *socket, char **tagp, json_object **op,
                    void **datap, int *lenp, int flags)
{
    zmsg_t *msg = NULL;

    if ((flags & ZMQ_DONTWAIT) && !zsocket_poll (socket, 0)) {
        errno = EAGAIN; 
        return -1;
    }
    if (!(msg = zmsg_recv (socket)))
        goto error;
    if (cmb_msg_decode (msg, tagp, op, datap, lenp) < 0)
        goto error;
    zmsg_destroy (&msg);
    return 0;
error:
    if (msg)
        zmsg_destroy (&msg);
    return -1;
}

int cmb_msg_recv_fd (int fd, char **tagp, json_object **op,
                     void **datap, int *lenp, int flags)
{
    zmsg_t *msg;

    msg = zmsg_recv_fd (fd, flags);
    if (!msg)
        goto error;
    if (cmb_msg_decode (msg, tagp, op, datap, lenp) < 0)
        goto error;
    zmsg_destroy (&msg);
    return 0;

error:
    if (msg)
        zmsg_destroy (&msg);
    return -1;
}

zmsg_t *cmb_msg_encode (char *tag, json_object *o, void *data, int len)
{
    zmsg_t *msg = NULL;

    if (!(msg = zmsg_new ()))
        err_exit ("zmsg_new");
    if (zmsg_addstr (msg, "%s", tag) < 0)
        err_exit ("zmsg_addstr");
    if (o) {
        if (zmsg_addstr (msg, "%s", json_object_to_json_string (o)) < 0)
            err_exit ("zmsg_addstr");
    }
    if (len > 0 && data != NULL) {
        assert (o != NULL);
        if (zmsg_addmem (msg, data, len) < 0)
            err_exit ("zmsg_addmem");
    }
    return msg;
}

void cmb_msg_send_long (void *sock, json_object *o, void *data, int len,
                        const char *fmt, ...)
{
    va_list ap;
    zmsg_t *msg;
    char *tag;
    int n;

    va_start (ap, fmt);
    n = vasprintf (&tag, fmt, ap);
    va_end (ap);
    if (n < 0)
        err_exit ("vasprintf");
   
    msg = cmb_msg_encode (tag, o, data, len);
    free (tag);
    if (zmsg_send (&msg, sock) < 0)
        err_exit ("zmsg_send");
}

void cmb_msg_send (void *sock, json_object *o, const char *fmt, ...)
{
    va_list ap;
    zmsg_t *msg;
    char *tag;
    int n;

    va_start (ap, fmt);
    n = vasprintf (&tag, fmt, ap);
    va_end (ap);
    if (n < 0)
        err_exit ("vasprintf");
   
    msg = cmb_msg_encode (tag, o, NULL, 0);
    free (tag);
    if (zmsg_send (&msg, sock) < 0)
        err_exit ("zmsg_send");
}

int cmb_msg_send_long_fd (int fd, json_object *o, void *data, int len,
                          const char *fmt, ...)
{
    va_list ap;
    zmsg_t *msg = NULL;
    char *tag = NULL;
    int n;

    va_start (ap, fmt);
    n = vasprintf (&tag, fmt, ap);
    va_end (ap);
    if (n < 0)
        err_exit ("vasprintf");

    msg = cmb_msg_encode (tag, o, data, len);
    if (zmsg_send_fd (fd, &msg) < 0) /* destroys msg on succes */
        goto error;
    free (tag);
    return 0;
error:
    if (msg)
        zmsg_destroy (&msg);
    if (tag)
        free (tag);
    return -1; 
}

int cmb_msg_send_fd (int fd, json_object *o, const char *fmt, ...)
{
    va_list ap;
    zmsg_t *msg = NULL;
    char *tag = NULL;
    int n;

    va_start (ap, fmt);
    n = vasprintf (&tag, fmt, ap);
    va_end (ap);
    if (n < 0)
        err_exit ("vasprintf");
   
    msg = cmb_msg_encode (tag, o, NULL, 0);
    if (zmsg_send_fd (fd, &msg) < 0) /* destroys msg on succes */
        goto error;
    free (tag);
    return 0;
error:
    if (msg)
        zmsg_destroy (&msg);
    if (tag)
        free (tag);
    return -1;
}

bool cmb_msg_match (zmsg_t *zmsg, const char *tag)
{
    zframe_t *zf = _tag_frame (zmsg);

    if (!zf)
        msg_exit ("cmb_msg_match: no tag in message");
    return zframe_streq (zf, tag);
}

bool cmb_msg_match_substr (zmsg_t *zmsg, const char *tag, char **restp)
{
    int taglen = strlen (tag);
    zframe_t *zf = _tag_frame (zmsg);
    char *ztag;
    int ztaglen;

    if (!zf)
        msg_exit ("cmb_msg_match: no tag in message");
    if (!(ztag = zframe_strdup (zf)))
        oom ();
    ztaglen = strlen (ztag); 
    if (ztaglen >= taglen && strncmp (tag, ztag, taglen) == 0) {
        if (restp) {
            memmove (ztag, ztag + taglen, ztaglen - taglen);
            *restp = ztag;
        } else
            free (ztag);
        return true;
    }
    free (ztag);
    return false;
}

/* extract the first address in the envelope (sender uuid) */
char *cmb_msg_sender (zmsg_t *zmsg)
{
    zframe_t *zf = _sender_frame (zmsg);
    if (!zf) {
        msg ("cmb_msg_sender: empty envelope");
        return NULL;
    }
    return zframe_strdup (zf); /* caller must free */
}

/* Append .NAK to the tag portion of message.
 * But otherwise leave the message and the envelope unchanged.
 * This indicates that the addressed plugin is not loaded.
 */
int cmb_msg_rep_nak (zmsg_t *zmsg)
{
    zframe_t *zf = _tag_frame (zmsg);
    char *tag = NULL, *newtag = NULL;

    if (!zf) {
        msg ("cmb_msg_makenak: no message tag");
        return -1;
    }
    tag = zframe_strdup (zf);
    if (!tag)
        goto error;
    if (asprintf (&newtag, "%s.NAK", tag) < 0)
        goto error;
    /* N.B. calls zmq_msg_init_size internally with unchecked return value */
    zframe_reset (zf, newtag, strlen (newtag));
    free (newtag);
    free (tag); 
    return 0;
error:
    if (tag)
        free (tag);
    if (newtag)
        free (newtag); 
    return -1;
}

/* Replace JSON portion of message.
 */
int cmb_msg_rep_json (zmsg_t *zmsg, json_object *o)
{
    const char *json = json_object_to_json_string (o);
    zframe_t *zf = _json_frame (zmsg);

    if (!zf) {
        msg ("cmb_msg_makenak: no json frame");
        return -1;
    }
    /* N.B. calls zmq_msg_init_size internally with unchecked return value */
    zframe_reset (zf, json, strlen (json));
    return 0;
}

int cmb_msg_datacpy (zmsg_t *zmsg, char *buf, int len)
{
    zframe_t *zf = _data_frame (zmsg);

    if (!zf) {
        msg ("cmb_msg_makenak: no data frame");
        return -1;
    }
    if (zframe_size (zf) > len) {
        msg ("%s: buffer too small", __FUNCTION__);
        return -1;
    }
    memcpy (buf, zframe_data (zf), zframe_size (zf));
    return zframe_size (zf);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

