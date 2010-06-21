/*
  Copyright (c) 2010 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "client.h"
#include "xlator.h"
#include "defaults.h"
#include "glusterfs.h"
#include "msg-xdr.h"
#include "statedump.h"
#include "compat-errno.h"

extern rpc_clnt_prog_t clnt3_1_fop_prog;
extern rpc_clnt_prog_t clnt3_1_mgmt_prog;

/* Handshake */

void
rpc_client_ping_timer_expired (void *data)
{
        rpc_transport_t         *trans              = NULL;
        rpc_clnt_connection_t   *conn               = NULL;
        int                      disconnect         = 0;
        int                      transport_activity = 0;
        struct timeval           timeout            = {0, };
        struct timeval           current            = {0, };
        struct rpc_clnt         *clnt               = NULL;
        xlator_t                *this               = NULL;
        clnt_conf_t             *conf               = NULL;

        if (!data) {
                goto out;
        }

        this = data;
        conf = this->private;

        conn = &conf->rpc->conn;
        trans = conn->trans;

        if (!clnt || !trans) {
                goto out;
        }

        pthread_mutex_lock (&conn->lock);
        {
                if (conn->ping_timer)
                        gf_timer_call_cancel (this->ctx,
                                              conn->ping_timer);
                gettimeofday (&current, NULL);

                if (((current.tv_sec - conn->last_received.tv_sec) <
                     conf->opt.ping_timeout)
                    || ((current.tv_sec - conn->last_sent.tv_sec) <
                        conf->opt.ping_timeout)) {
                        transport_activity = 1;
                }

                if (transport_activity) {
                        gf_log (trans->name, GF_LOG_TRACE,
                                "ping timer expired but transport activity "
                                "detected - not bailing transport");
                        timeout.tv_sec = conf->opt.ping_timeout;
                        timeout.tv_usec = 0;

                        conn->ping_timer =
                                gf_timer_call_after (this->ctx, timeout,
                                                     rpc_client_ping_timer_expired,
                                                     (void *) this);
                        if (conn->ping_timer == NULL)
                                gf_log (trans->name, GF_LOG_DEBUG,
                                        "unable to setup timer");

                } else {
                        conn->ping_started = 0;
                        conn->ping_timer = NULL;
                        disconnect = 1;
                }
        }
        pthread_mutex_unlock (&conn->lock);

        if (disconnect) {
                gf_log (trans->name, GF_LOG_ERROR,
                        "Server %s has not responded in the last %d "
                        "seconds, disconnecting.",
                        conn->trans->peerinfo.identifier,
                        conf->opt.ping_timeout);

                rpc_transport_disconnect (conn->trans);
        }

out:
        return;
}


void
client_start_ping (void *data)
{
        xlator_t                *this        = NULL;
        clnt_conf_t             *conf        = NULL;
        rpc_clnt_connection_t   *conn        = NULL;
        int32_t                  ret         = -1;
        struct timeval           timeout     = {0, };
        call_frame_t            *frame       = NULL;
        int                      frame_count = 0;
        rpc_transport_t         *trans       = NULL;

        this = data;
        conf  = this->private;

        conn = &conf->rpc->conn;
        trans = conn->trans;

        if (conf->opt.ping_timeout == 0)
                return;

        pthread_mutex_lock (&conn->lock);
        {
                if (conn->ping_timer)
                        gf_timer_call_cancel (this->ctx, conn->ping_timer);

                conn->ping_timer = NULL;
                conn->ping_started = 0;

                if (conn->saved_frames)
                        /* treat the case where conn->saved_frames is NULL
                           as no pending frames */
                        frame_count = conn->saved_frames->count;

                if ((frame_count == 0) || !conn->connected) {
                        /* using goto looked ugly here,
                         * hence getting out this way */
                        /* unlock */
                        pthread_mutex_unlock (&conn->lock);
                        return;
                }

                if (frame_count < 0) {
                        gf_log (this->name, GF_LOG_DEBUG,
                                "saved_frames->count is %"PRId64,
                                conn->saved_frames->count);
                        conn->saved_frames->count = 0;
                }

                timeout.tv_sec = conf->opt.ping_timeout;
                timeout.tv_usec = 0;

                conn->ping_timer =
                        gf_timer_call_after (this->ctx, timeout,
                                             rpc_client_ping_timer_expired,
                                             (void *) this);

                if (conn->ping_timer == NULL) {
                        gf_log (this->name, GF_LOG_DEBUG,
                                "unable to setup timer");
                } else {
                        conn->ping_started = 1;
                }
        }
        pthread_mutex_unlock (&conn->lock);

        frame = create_frame (this, this->ctx->pool);
        if (!frame)
                goto fail;

        ret = client_submit_request (this, NULL, frame, conf->handshake,
                                     GF_HNDSK_PING, NULL, NULL);

        return;
fail:

        if (frame) {
                STACK_DESTROY (frame->root);
        }

        return;
}


int
client_ping_cbk (struct rpc_req *req, struct iovec *iov, int count,
                 void *myframe)
{
        xlator_t              *this    = NULL;
        rpc_clnt_connection_t *conn    = NULL;
        struct timeval         timeout = {0, };
        call_frame_t          *frame   = NULL;
        clnt_conf_t           *conf    = NULL;

        frame = myframe;

        this = frame->this;
        conf = this->private;
        conn = &conf->rpc->conn;

        if (req->rpc_status == -1) {
                /* timer expired and transport bailed out */
                gf_log (this->name, GF_LOG_DEBUG, "timer must have expired");
                goto out;
        }

        pthread_mutex_lock (&conn->lock);
        {
                timeout.tv_sec  = conf->opt.ping_timeout;
                timeout.tv_usec = 0;

                gf_timer_call_cancel (this->ctx,
                                      conn->ping_timer);

                conn->ping_timer =
                        gf_timer_call_after (this->ctx, timeout,
                                             client_start_ping, (void *)this);

                if (conn->ping_timer == NULL)
                        gf_log (this->name, GF_LOG_DEBUG,
                                "gf_timer_call_after() returned NULL");
        }
        pthread_mutex_unlock (&conn->lock);
out:
        STACK_DESTROY (frame->root);
        return 0;
}


int
client_getspec_cbk (struct rpc_req *req, struct iovec *iov, int count, void *myframe)
{
        gf_getspec_rsp           rsp   = {0,};
        call_frame_t            *frame = NULL;
        clnt_conf_t             *conf = NULL;
        int                      ret   = 0;
        char                     spec[(32*1024)] = {0,};

        frame = myframe;
        conf  = frame->this->private;

        if (-1 == req->rpc_status) {
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        rsp.spec = spec;
        ret = xdr_to_dump_version_rsp (*iov, &rsp);
        if (ret < 0) {
                gf_log ("", GF_LOG_ERROR, "error");
                rsp.op_ret   = -1;
                rsp.op_errno = EINVAL;
                goto out;
        }

        if (-1 == rsp.op_ret) {
                gf_log (frame->this->name, GF_LOG_ERROR,
                        "failed to get the 'volume file' from server");
                goto out;
        }

out:
        /* no _STRICT for mops */
        STACK_UNWIND_STRICT (getspec, frame, rsp.op_ret, rsp.op_errno, rsp.spec);
        return 0;
}


int
client_post_handshake (call_frame_t *frame, xlator_t *this)
{
        clnt_conf_t            *conf = NULL;
        clnt_fd_ctx_t          *tmp = NULL;
        clnt_fd_ctx_t          *fdctx = NULL;
        xlator_list_t          *parent = NULL;
        struct list_head        reopen_head;

        if (!this || !this->private)
                goto out;

        conf = this->private;
        INIT_LIST_HEAD (&reopen_head);

        pthread_mutex_lock (&conf->lock);
        {
                list_for_each_entry_safe (fdctx, tmp, &conf->saved_fds,
                                          sfd_pos) {
                        if (fdctx->remote_fd != -1)
                                continue;

                        list_del_init (&fdctx->sfd_pos);
                        list_add_tail (&fdctx->sfd_pos, &reopen_head);
                }
        }
        pthread_mutex_unlock (&conf->lock);

        list_for_each_entry_safe (fdctx, tmp, &reopen_head, sfd_pos) {
                list_del_init (&fdctx->sfd_pos);

                if (fdctx->is_dir)
                        protocol_client_reopendir (this, fdctx);
                else
                        protocol_client_reopen (this, fdctx);
        }

        parent = this->parents;

        while (parent) {
                xlator_notify (parent->xlator, GF_EVENT_CHILD_UP,
                               this);
                parent = parent->next;
        }

out:
        return 0;
}

int
client_setvolume_cbk (struct rpc_req *req, struct iovec *iov, int count, void *myframe)
{
        call_frame_t         *frame         = NULL;
        clnt_conf_t          *conf          = NULL;
        xlator_t             *this          = NULL;
        dict_t               *reply         = NULL;
        xlator_list_t        *parent        = NULL;
        char                 *process_uuid  = NULL;
        char                 *remote_error  = NULL;
        char                 *remote_subvol = NULL;
        rpc_transport_t      *peer_trans    = NULL;
        gf_setvolume_rsp      rsp           = {0,};
        uint64_t              peertrans_int = 0;
        int                   ret           = 0;
        int                   op_ret        = 0;
        int                   op_errno      = 0;

        frame = myframe;
        this  = frame->this;
        conf  = this->private;

        if (-1 == req->rpc_status) {
                op_ret = -1;
                op_errno = EINVAL;
                goto out;
        }

        ret = xdr_to_setvolume_rsp (*iov, &rsp);
        if (ret < 0) {
                gf_log ("", GF_LOG_ERROR, "error");
                op_errno = EINVAL;
                op_ret = -1;
                goto out;
        }
        op_ret   = rsp.op_ret;
        op_errno = gf_error_to_errno (rsp.op_errno);
        if (-1 == rsp.op_ret) {
                gf_log (frame->this->name, GF_LOG_WARNING,
                        "failed to set the volume");
        }

        reply = dict_new ();
        if (!reply)
                goto out;

        if (rsp.dict.dict_len) {
                ret = dict_unserialize (rsp.dict.dict_val,
                                        rsp.dict.dict_len, &reply);
                if (ret < 0) {
                        gf_log (frame->this->name, GF_LOG_DEBUG,
                                "failed to unserialize buffer to dict");
                        goto out;
                }
        }

        ret = dict_get_str (reply, "ERROR", &remote_error);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "failed to get ERROR string from reply dict");
        }

        ret = dict_get_str (reply, "process-uuid", &process_uuid);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "failed to get 'process-uuid' from reply dict");
        }

        if (op_ret < 0) {
                gf_log (this->name, GF_LOG_ERROR,
                        "SETVOLUME on remote-host failed: %s",
                        remote_error ? remote_error : strerror (op_errno));
                errno = op_errno;
                if (op_errno == ESTALE) {
                        parent = this->parents;
                        while (parent) {
                                xlator_notify (parent->xlator,
                                               GF_EVENT_VOLFILE_MODIFIED,
                                               this);
                                parent = parent->next;
                        }
                }
                goto out;
        }
        ret = dict_get_str (this->options, "remote-subvolume",
                            &remote_subvol);
        if (!remote_subvol)
                goto out;

        if (process_uuid &&
            !strcmp (this->ctx->process_uuid, process_uuid)) {
                ret = dict_get_uint64 (reply, "transport-ptr",
                                       &peertrans_int);

                peer_trans = (void *) (long) (peertrans_int);

                gf_log (this->name, GF_LOG_WARNING,
                        "attaching to the local volume '%s'",
                        remote_subvol);

                if (req->conn) {
                        /* TODO: Some issues with this logic at present */
                        //rpc_transport_setpeer (req->conn->trans, peer_trans);
                }
        }

        gf_log (this->name, GF_LOG_NORMAL,
                "Connected to %s, attached to remote volume '%s'.",
                conf->rpc->conn.trans->peerinfo.identifier,
                remote_subvol);

        rpc_clnt_set_connected (&conf->rpc->conn);

        op_ret = 0;
        conf->connecting = 0;

        /* TODO: more to test */
        client_post_handshake (frame, frame->this);

out:

        if (-1 == op_ret) {
                /* Let the connection/re-connection happen in
                 * background, for now, don't hang here,
                 * tell the parents that i am all ok..
                 */
                parent = this->parents;
                while (parent) {
                        xlator_notify (parent->xlator,
                                       GF_EVENT_CHILD_CONNECTING, this);
                        parent = parent->next;
                }

                conf->connecting= 1;
        }

        if (rsp.dict.dict_val)
                free (rsp.dict.dict_val);

        STACK_DESTROY (frame->root);

        if (reply)
                dict_unref (reply);

        return 0;
}

int
client_setvolume (xlator_t *this, struct rpc_clnt *rpc)
{
        int               ret             = 0;
        gf_setvolume_req  req             = {0,};
        call_frame_t     *fr              = NULL;
        char             *process_uuid_xl = NULL;
        clnt_conf_t      *conf            = NULL;
        dict_t           *options         = NULL;

        options = this->options;
        conf    = this->private;

        if (conf->fops || !dict_get (options, "fops-version")) {
                ret = dict_set_int32 (options, "fops-version",
                                      conf->fops->prognum);
                if (ret < 0) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "failed to set version-fops(%d) in handshake msg",
                                conf->fops->prognum);
                        goto fail;
                }
        }

        if (conf->mgmt) {
                ret = dict_set_int32 (options, "mgmt-version", conf->mgmt->prognum);
                if (ret < 0) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "failed to set version-mgmt(%d) in handshake msg",
                                conf->mgmt->prognum);
                        goto fail;
                }
        }

        ret = gf_asprintf (&process_uuid_xl, "%s-%s", this->ctx->process_uuid,
                           this->name);
        if (-1 == ret) {
                gf_log (this->name, GF_LOG_ERROR,
                        "asprintf failed while setting process_uuid");
                goto fail;
        }
        ret = dict_set_dynstr (options, "process-uuid", process_uuid_xl);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR,
                        "failed to set process-uuid(%s) in handshake msg",
                        process_uuid_xl);
                goto fail;
        }

        if (this->ctx->cmd_args.volfile_server) {
                if (this->ctx->cmd_args.volfile_id)
                        ret = dict_set_str (options, "volfile-key",
                                            this->ctx->cmd_args.volfile_id);
                ret = dict_set_uint32 (options, "volfile-checksum",
                                       this->graph->volfile_checksum);
        }

        req.dict.dict_len = dict_serialized_length (options);
        if (req.dict.dict_len < 0) {
                gf_log (this->name, GF_LOG_ERROR,
                        "failed to get serialized length of dict");
                ret = -1;
                goto fail;
        }
        req.dict.dict_val = GF_CALLOC (1, req.dict.dict_len, 0);
        ret = dict_serialize (options, req.dict.dict_val);
        if (ret < 0) {
                gf_log (this->name, GF_LOG_ERROR,
                        "failed to serialize dictionary");
                goto fail;
        }

        fr  = create_frame (this, this->ctx->pool);
        if (!fr)
                goto fail;

        ret = client_submit_request (this, &req, fr, conf->handshake,
                                     GF_HNDSK_SETVOLUME, NULL, xdr_from_setvolume_req);

fail:
        if (req.dict.dict_val)
                GF_FREE (req.dict.dict_val);

        return ret;
}

int
select_server_supported_programs (xlator_t *this, char *msg)
{
        clnt_conf_t *conf        = NULL;
        char        *tmp_str     = NULL;
        char        *prog_str    = NULL;
        char        *dup_str     = NULL;
        char        *tmp_str1    = NULL;
        char        *tmp_msg     = NULL;
        char        *progname    = NULL;
        char        *progver_str = NULL;
        char        *prognum_str = NULL;
        int          ret         = -1;
        int          progver     = 0;
        int          prognum     = 0;

        if (!this || !msg)
                goto out;

        conf = this->private;

        /* Reply in "Name:Program-Number:Program-Version,..." format */
        tmp_msg = gf_strdup (msg);
        prog_str = strtok_r (tmp_msg, ",", &tmp_str);
        while (prog_str) {
                dup_str = gf_strdup (prog_str);

                progname = strtok_r (dup_str, ":", &tmp_str1);
                prognum_str = strtok_r (NULL, ":", &tmp_str1);
                if (!prognum_str) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "Supported versions not formatted");
                        goto out;
                }
                sscanf (prognum_str, "%d", &prognum);
                progver_str = strtok_r (NULL, ":", &tmp_str1);
                if (!progver_str) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "Supported versions not formatted");
                        goto out;
                }
                sscanf (progver_str, "%d", &progver);

                /* Select 'programs' */
                if ((clnt3_1_fop_prog.prognum == prognum) &&
                    (clnt3_1_fop_prog.progver == progver)) {
                        conf->fops = &clnt3_1_fop_prog;
                        gf_log (this->name, GF_LOG_INFO,
                                "Using Program %s, Num (%s), Version (%s)",
                                progname, prognum_str, progver_str);
                        ret = 0;
                }
                if ((clnt3_1_mgmt_prog.prognum == prognum) &&
                    (clnt3_1_mgmt_prog.progver == progver)) {
                        conf->mgmt = &clnt3_1_mgmt_prog;
                        gf_log (this->name, GF_LOG_INFO,
                                "Using Program %s, Num (%s), Version (%s)",
                                progname, prognum_str, progver_str);
                        ret = 0;
                }

                prog_str = strtok_r (NULL, ",", &tmp_str);
                GF_FREE (dup_str);
        }

        if (ret) {
                gf_log (this->name, GF_LOG_ERROR,
                        "none of the server versions are supported by client");
        }
        ret = 0;
out:
        if (tmp_msg)
                GF_FREE (tmp_msg);
        return ret;
}

int
client_dump_version_cbk (struct rpc_req *req, struct iovec *iov, int count, void *myframe)
{
        gf_dump_version_rsp  rsp   = {0,};
        call_frame_t            *frame = NULL;
        clnt_conf_t             *conf = NULL;
        int                      ret   = 0;

        frame = myframe;
        conf  = frame->this->private;

        if (-1 == req->rpc_status) {
                gf_log ("", 1, "some error, retry again later");
                goto out;
        }

        ret = xdr_to_dump_version_rsp (*iov, &rsp);
        if (ret < 0) {
                gf_log ("", GF_LOG_ERROR, "error");
                goto out;
        }
        if (-1 == rsp.op_ret) {
                gf_log (frame->this->name, GF_LOG_ERROR,
                        "failed to get the 'versions' from server");
                goto out;
        }

        /* Check for the proper version string */
        /* Reply in "Name:Program-Number:Program-Version,..." format */
        ret = select_server_supported_programs (frame->this,
                                                rsp.msg.msg_val);
        if (ret) {
                gf_log (frame->this->name, GF_LOG_ERROR,
                        "Server versions are not present in this "
                        "release (%s)", rsp.msg.msg_val);
                goto out;
        }

        client_setvolume (frame->this, conf->rpc);

out:
        /* don't use GF_FREE, buffer was allocated by libc */
        if (rsp.msg.msg_val) {
                free (rsp.msg.msg_val);
        }

        STACK_DESTROY (frame->root);
        return ret;
}

int
client_handshake (xlator_t *this, struct rpc_clnt *rpc)
{
        call_frame_t        *frame = NULL;
        clnt_conf_t         *conf  = NULL;
        gf_dump_version_req  req   = {0,};
        int                  ret   = 0;

        conf = this->private;
        if (!conf->handshake)
                goto out;

        frame = create_frame (this, this->ctx->pool);
        if (!frame)
                goto out;

        req.key = "fop-handshake";
        req.gfs_id = 123456;
        ret = client_submit_request (this, &req, frame, conf->handshake,
                                     GF_HNDSK_DUMP_VERSION,
                                     NULL, xdr_from_dump_version_req);

out:
        return ret;
}


/* */
/* This table should ideally remain same irrespective of versions */
static rpc_clnt_procedure_t clnt_handshake_actors[] = {
        [GF_HNDSK_NULL]         = { "NULL", NULL, NULL},
        [GF_HNDSK_DUMP_VERSION] = { "VERSION", NULL, client_dump_version_cbk},
        [GF_HNDSK_SETVOLUME]    = { "SETVOLUME", NULL,  client_setvolume_cbk},
        [GF_HNDSK_GETSPEC]      = { "GETSPEC", NULL, client_getspec_cbk },
        [GF_HNDSK_PING]         = { "PING", NULL, client_ping_cbk },
};

rpc_clnt_prog_t clnt_handshake_prog = {
        .progname = "GlusterFS Handshake",
        .prognum  = GLUSTER_HNDSK_PROGRAM,
        .progver  = GLUSTER_HNDSK_VERSION,
        .actor    = clnt_handshake_actors,
        .numproc  = (sizeof (*clnt_handshake_actors) /
                     sizeof (rpc_clnt_procedure_t)),
};
