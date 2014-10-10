/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/* vim: set ts=8 sts=4 sw=4 noexpandtab : */
/*
 *
 */



#include "mpid_nem_impl.h"
#include "tofu_impl.h"

//#define MPID_NEM_TOFU_DEBUG_SEND
#ifdef MPID_NEM_TOFU_DEBUG_SEND
#define dprintf printf
#else
#define dprintf(...)
#endif

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_isend
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_isend(struct MPIDI_VC *vc, const void *buf, int count, MPI_Datatype datatype,
                        int dest, int tag, MPID_Comm *comm, int context_offset,
                        struct MPID_Request **req_out)
{
    int mpi_errno = MPI_SUCCESS, llc_errno;
    int dt_contig;
    MPIDI_msg_sz_t data_sz;
    MPID_Datatype *dt_ptr;
    MPI_Aint dt_true_lb;
    int i;

    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_ISEND);
    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_ISEND);

    dprintf("tofu_isend,%d->%d,buf=%p,count=%d,datatype=%08x,dest=%d,tag=%08x,comm=%p,context_offset=%d\n",
            MPIDI_Process.my_pg_rank, vc->pg_rank, buf, count, datatype, dest, tag, comm, context_offset);

    int LLC_my_rank;
    LLC_comm_rank(LLC_COMM_MPICH, &LLC_my_rank);
    dprintf("tofu_isend,LLC_my_rank=%d\n", LLC_my_rank);

    struct MPID_Request * sreq = MPID_Request_create();
    MPIU_Assert(sreq != NULL);
    MPIU_Object_set_ref(sreq, 2);
    sreq->kind = MPID_REQUEST_SEND;

    /* Used in tofullc_poll --> MPID_nem_tofu_send_handler */
    sreq->ch.vc = vc;
    sreq->dev.OnDataAvail = 0;
    /* Don't save iov_offset because it's not used. */

    /* Save it because it's used in send_handler */
    sreq->dev.datatype = datatype;

    dprintf("tofu_isend,remote_endpoint_addr=%ld\n", VC_FIELD(vc, remote_endpoint_addr));

    LLC_cmd_t *cmd = LLC_cmd_alloc(1);
    cmd[0].opcode = LLC_OPCODE_SEND;
    cmd[0].comm = LLC_COMM_MPICH;
    cmd[0].rank = VC_FIELD(vc, remote_endpoint_addr);
    cmd[0].req_id = cmd;
    
    /* Prepare bit-vector to perform tag-match. We use the same bit-vector as in CH3 layer. */
    /* See src/mpid/ch3/src/mpid_isend.c */
    *(int32_t*)((uint8_t*)&cmd[0].tag) = tag;
 	*(MPIR_Context_id_t*)((uint8_t*)&cmd[0].tag + sizeof(int32_t)) =
        comm->context_id + context_offset;
    MPIU_Assert(sizeof(LLC_tag_t) >= sizeof(int32_t) + sizeof(MPIR_Context_id_t));
    memset((uint8_t*)&cmd[0].tag + sizeof(int32_t) + sizeof(MPIR_Context_id_t),
           0, sizeof(LLC_tag_t) - sizeof(int32_t) - sizeof(MPIR_Context_id_t));

    dprintf("tofu_isend,tag=");
    for(i = 0; i < sizeof(LLC_tag_t); i++) {
        dprintf("%02x", (int)*((uint8_t*)&cmd[0].tag + i));
    }
    dprintf("\n");

    /* Prepare RDMA-write from buffer */
    MPIDI_Datatype_get_info(count, datatype, dt_contig, data_sz, dt_ptr,
                            dt_true_lb);
    dprintf("tofu_isend,dt_contig=%d,data_sz=%ld\n",
            dt_contig, data_sz);


    const void *write_from_buf;
    if (dt_contig) {
        write_from_buf = buf + dt_true_lb;
    }
    else {
        /* See MPIDI_CH3_EagerNoncontigSend (in ch3u_eager.c) */
        struct MPID_Segment *segment_ptr = MPID_Segment_alloc();
        MPIU_ERR_CHKANDJUMP(!segment_ptr, mpi_errno, MPI_ERR_OTHER, "**outofmemory");

        MPID_Segment_init(buf, count, datatype, segment_ptr, 0);
        MPIDI_msg_sz_t segment_first = 0;
        MPIDI_msg_sz_t segment_size = data_sz;
        MPIDI_msg_sz_t last = segment_size;
        MPIU_Assert(last > 0);
        REQ_FIELD(sreq, pack_buf) = MPIU_Malloc((size_t) data_sz);
        MPIU_ERR_CHKANDJUMP(!REQ_FIELD(sreq, pack_buf), mpi_errno, MPI_ERR_OTHER,
                            "**outofmemory");
        MPID_Segment_pack(segment_ptr, segment_first, &last,
                          (char *) (REQ_FIELD(sreq, pack_buf)));
        MPIU_Assert(last == data_sz);
        write_from_buf = REQ_FIELD(sreq, pack_buf);
    }

    cmd[0].iov_local = LLC_iov_alloc(1);
    cmd[0].iov_local[0].addr = (uint64_t)write_from_buf;
    cmd[0].iov_local[0].length = data_sz;
    cmd[0].niov_local = 1;
    
    cmd[0].iov_remote = LLC_iov_alloc(1);
    cmd[0].iov_remote[0].addr = 0;
    cmd[0].iov_remote[0].length = data_sz;
    cmd[0].niov_remote = 1;
    
    ((struct llctofu_cmd_area *)cmd[0].usr_area)->cbarg = sreq;
    ((struct llctofu_cmd_area *)cmd[0].usr_area)->raddr = VC_FIELD(vc, remote_endpoint_addr);
    
    llc_errno = LLC_post(cmd, 1);
    MPIU_ERR_CHKANDJUMP(llc_errno != LLC_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**LLC_post");

  fn_exit:
    *req_out = sreq;
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_ISEND);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_iStartContigMsg
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_iStartContigMsg(MPIDI_VC_t *vc, void *hdr, MPIDI_msg_sz_t hdr_sz, void *data, MPIDI_msg_sz_t data_sz, MPID_Request **sreq_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *sreq = NULL;
    MPID_nem_tofu_vc_area *vc_tofu = 0;
    int need_to_queue = 0;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_ISTARTCONTIGMSG);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_ISTARTCONTIGMSG);

    dprintf("tofu_iStartContigMsg,%d->%d,hdr=%p,hdr_sz=%ld,data=%p,data_sz=%ld\n",
            MPIDI_Process.my_pg_rank, vc->pg_rank, hdr, hdr_sz, data, data_sz);

    MPIU_Assert(hdr_sz <= sizeof(MPIDI_CH3_Pkt_t));
    MPIU_DBG_MSG(CH3_CHANNEL, VERBOSE, "tofu_iStartContigMsg");
    MPIDI_DBG_Print_packet((MPIDI_CH3_Pkt_t *)hdr);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"vc.pg_rank = %d", vc->pg_rank);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"my_pg_rank = %d", MPIDI_Process.my_pg_rank);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"hdr_sz     = %d", (int)hdr_sz);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"data_sz    = %d", (int)data_sz);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"hdr type   = %d", ((MPIDI_CH3_Pkt_t *)hdr)->type);

    /* create a request */
    sreq = MPID_Request_create();
    MPIU_Assert (sreq != NULL);
    MPIU_Object_set_ref(sreq, 2);
    sreq->kind = MPID_REQUEST_SEND;

    sreq->ch.vc = vc;
    sreq->dev.OnDataAvail = 0;
    sreq->dev.iov_offset = 0;

    /* sreq: src/mpid/ch3/include/mpidpre.h */
    sreq->dev.pending_pkt = *(MPIDI_CH3_Pkt_t *)hdr;
    sreq->dev.iov[0].MPID_IOV_BUF =
	(MPID_IOV_BUF_CAST) &sreq->dev.pending_pkt;
    sreq->dev.iov[0].MPID_IOV_LEN = sizeof (MPIDI_CH3_Pkt_t);
    sreq->dev.iov_count = 1;
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"IOV_LEN    = %d", (int)sreq->dev.iov[0].MPID_IOV_LEN);
    if (data_sz > 0) {
	sreq->dev.iov[1].MPID_IOV_BUF = data;
	sreq->dev.iov[1].MPID_IOV_LEN = data_sz;
	sreq->dev.iov_count = 2;
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "IOV_LEN    = %d", (int)sreq->dev.iov[0].MPID_IOV_LEN);
    }

    vc_tofu = VC_TOFU(vc);
    if ( ! MPIDI_CH3I_Sendq_empty(vc_tofu->send_queue) ) {
	need_to_queue = 1;
	goto queue_it;
    }

    {
	int ret;

	ret = llctofu_writev(vc_tofu->endpoint,
		vc_tofu->remote_endpoint_addr,
		sreq->dev.iov, sreq->dev.iov_count,
		sreq, &REQ_TOFU(sreq)->cmds);
	if (ret < 0) {
	    mpi_errno = MPI_ERR_OTHER;
	    MPIU_ERR_POP(mpi_errno);
	}
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "IOV_LEN    = %d", (int)sreq->dev.iov[0].MPID_IOV_LEN);
	if ( ! MPIDI_nem_tofu_Rqst_iov_update(sreq, ret) ) {
	    need_to_queue = 2; /* YYY */
	}
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "IOV_LEN    = %d", (int)sreq->dev.iov[0].MPID_IOV_LEN);
    }

queue_it:
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"need_to_que  %d", need_to_queue);
    if (need_to_queue > 0) {
	MPIDI_CH3I_Sendq_enqueue(&vc_tofu->send_queue, sreq);
    }

 fn_exit:
    *sreq_ptr = sreq;
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_ISTARTCONTIGMSG);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_iSendContig
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_iSendContig(MPIDI_VC_t *vc, MPID_Request *sreq, void *hdr, MPIDI_msg_sz_t hdr_sz, void *data, MPIDI_msg_sz_t data_sz)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_nem_tofu_vc_area *vc_tofu = 0;
    int need_to_queue = 0;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_ISENDCONTIGMSG);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_ISENDCONTIGMSG);

    dprintf("tofu_iSendConitig,sreq=%p,hdr=%p,hdr_sz=%ld,data=%p,data_sz=%ld\n",
            sreq, hdr, hdr_sz, data, data_sz);

    MPIU_Assert(hdr_sz <= sizeof(MPIDI_CH3_Pkt_t));
    MPIU_DBG_MSG(CH3_CHANNEL, VERBOSE, "tofu_iSendContig");
    MPIDI_DBG_Print_packet((MPIDI_CH3_Pkt_t *)hdr);
    MPIU_DBG_PKT(vc, hdr, "isendcontig");
    {
	MPIDI_CH3_Pkt_t *pkt = (MPIDI_CH3_Pkt_t *)hdr;

	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "pkt->type  = %d", pkt->type);
    }

    MPIU_Assert (sreq != NULL);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"OnDataAvail= %p", sreq->dev.OnDataAvail);
    sreq->ch.vc = vc;
    sreq->dev.iov_offset = 0;

    /* sreq: src/mpid/ch3/include/mpidpre.h */
    sreq->dev.pending_pkt = *(MPIDI_CH3_Pkt_t *)hdr;
    sreq->dev.iov[0].MPID_IOV_BUF =
	(MPID_IOV_BUF_CAST) &sreq->dev.pending_pkt;
    sreq->dev.iov[0].MPID_IOV_LEN = sizeof (MPIDI_CH3_Pkt_t);
    sreq->dev.iov_count = 1;
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"IOV_LEN    = %d", (int)sreq->dev.iov[0].MPID_IOV_LEN);
    if (data_sz > 0) {
	sreq->dev.iov[1].MPID_IOV_BUF = data;
	sreq->dev.iov[1].MPID_IOV_LEN = data_sz;
	sreq->dev.iov_count = 2;
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "IOV_LEN    = %d", (int)sreq->dev.iov[1].MPID_IOV_LEN);
    }

    vc_tofu = VC_TOFU(vc);
    if ( ! MPIDI_CH3I_Sendq_empty(vc_tofu->send_queue) ) {
	need_to_queue = 1;
	goto queue_it;
    }

    {
	int ret;

	ret = llctofu_writev(vc_tofu->endpoint,
		vc_tofu->remote_endpoint_addr,
		sreq->dev.iov, sreq->dev.iov_count,
		sreq, &REQ_TOFU(sreq)->cmds);
	if (ret < 0) {
	    mpi_errno = MPI_ERR_OTHER;
	    MPIU_ERR_POP(mpi_errno);
	}
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "WRITEV()   = %d", ret);
	if ( ! MPIDI_nem_tofu_Rqst_iov_update(sreq, ret) ) {
	    need_to_queue = 2; /* YYY */
	}
    }

queue_it:
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"need_to_que  %d", need_to_queue);
    if (need_to_queue > 0) {
	MPIDI_CH3I_Sendq_enqueue(&vc_tofu->send_queue, sreq);
    }

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_ISENDCONTIGMSG);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_SendNoncontig
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_SendNoncontig(MPIDI_VC_t *vc, MPID_Request *sreq, void *hdr, MPIDI_msg_sz_t hdr_sz)
{
    int mpi_errno = MPI_SUCCESS;
    /* MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_SENDNONCONTIG); */

    /* MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_SENDNONCONTIG); */
    MPIU_DBG_MSG(CH3_CHANNEL, VERBOSE, "tofu_SendNoncontig");

    MPIU_Assert(hdr_sz <= sizeof(MPIDI_CH3_Pkt_t));

 fn_exit:
    /* MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_SENDNONCONTIG); */
    return mpi_errno;
    //fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_send_queued
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_send_queued(MPIDI_VC_t * vc, rque_t *send_queue)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_nem_tofu_vc_area *vc_tofu;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_SEND_QUEUED);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_SEND_QUEUED);

    MPIU_Assert(vc != NULL);
    vc_tofu = VC_TOFU(vc);
    MPIU_Assert(vc_tofu != NULL);

    while ( ! MPIDI_CH3I_Sendq_empty(*send_queue) ) {
	ssize_t ret = 0;
	MPID_Request *sreq;
	void *endpt = vc_tofu->endpoint;
	MPID_IOV *iovs;
	int niov;

	sreq = MPIDI_CH3I_Sendq_head(*send_queue);
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE, "sreq %p", sreq);

	if (mpi_errno == MPI_SUCCESS) {
	    iovs = &sreq->dev.iov[sreq->dev.iov_offset];
	    niov = sreq->dev.iov_count;

	    ret = llctofu_writev(endpt, vc_tofu->remote_endpoint_addr,
		    iovs, niov, sreq, &REQ_TOFU(sreq)->cmds);
	    if (ret < 0) {
		mpi_errno = MPI_ERR_OTHER;
	    }
	}
	if (mpi_errno != MPI_SUCCESS) {
	    MPIDI_CH3I_Sendq_dequeue(send_queue, &sreq);
	    sreq->status.MPI_ERROR = mpi_errno;

	    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
		"OnDataAvail = %p", sreq->dev.OnDataAvail);
	    MPIDI_CH3U_Request_complete(sreq);
	    continue;
	}
	if ( ! MPIDI_nem_tofu_Rqst_iov_update(sreq, ret) ) {
	    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE, "skip %p", sreq);
	    break;
	}
	MPIDI_CH3I_Sendq_dequeue(send_queue, &sreq);
    }

  fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_SEND_QUEUED);
    return mpi_errno;
    //fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_nem_tofu_Rqst_iov_update
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPIDI_nem_tofu_Rqst_iov_update(MPID_Request *mreq, MPIDI_msg_sz_t consume)
{
    int ret = TRUE;
    /* MPIDI_msg_sz_t oconsume = consume; */
    int iv, nv;
    MPIDI_STATE_DECL(MPID_STATE_MPIDI_NEM_TOFU_RQST_IOV_UPDATE);

    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_NEM_TOFU_RQST_IOV_UPDATE);

    MPIU_Assert(consume >= 0);

    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"iov_update() : consume    %d", (int)consume);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"iov_update() : iov_count  %d", mreq->dev.iov_count);

    nv = mreq->dev.iov_count;
    for (iv = mreq->dev.iov_offset; iv < nv; iv++) {
	MPID_IOV *iov = &mreq->dev.iov[iv];

	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "iov_update() : iov[iv]    %d", iv);
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "iov_update() : consume b  %d", (int)consume);
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "iov_update() : iov_len b  %d", (int)iov->MPID_IOV_LEN);
	if (iov->MPID_IOV_LEN > consume) {
	    iov->MPID_IOV_BUF = ((char *) iov->MPID_IOV_BUF) + consume;
	    iov->MPID_IOV_LEN -= consume;
	    consume = 0;
	    ret = FALSE;
	    break;
	}
	consume -= iov->MPID_IOV_LEN;
	iov->MPID_IOV_LEN = 0;
    }
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"iov_update() : consume %d", (int)consume);

    mreq->dev.iov_count = nv - iv;
    mreq->dev.iov_offset = iv;

    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"iov_update() : iov_offset %d", mreq->dev.iov_offset);
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	"iov_update() = %d", ret);

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_NEM_TOFU_RQST_IOV_UPDATE);
    return ret;
}

ssize_t llctofu_writev(void *endpt, uint64_t raddr,
    const struct iovec *iovs, int niov, void *cbarg, void **vpp_reqid)
{
    ssize_t nw = 0;
    LLC_cmd_t *lcmd = 0;

    dprintf("writev,raddr=%ld,niov=%d,sreq=%p", raddr, niov, cbarg);

    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE, "llctofu_writev(%d)", (int)raddr);
    {
        uint8_t *buff = 0;
        uint32_t bsiz;

        {
            int iv, nv = niov;
            bsiz = 0;
            for (iv = 0; iv < nv; iv++) {
                size_t len = iovs[iv].iov_len;

                if (len <= 0) {
                    continue;
                }
                bsiz += len;
            }
            if (bsiz > 0) {
                buff = MPIU_Malloc(bsiz + sizeof(MPID_nem_tofu_netmod_hdr_t));
                if (buff == 0) {
                    nw = -1; /* ENOMEM */
                    goto bad;
                }
            }
        }
        
        lcmd = LLC_cmd_alloc(1);
        if (lcmd == 0) {
            if (buff != 0) { MPIU_Free(buff); buff = 0; }
            nw = -1; /* ENOMEM */
            goto bad;
        }
        lcmd[0].iov_local = LLC_iov_alloc(1);
        lcmd[0].iov_remote = LLC_iov_alloc(1);

        UNSOLICITED_NUM_INC(cbarg);
        lcmd->opcode = LLC_OPCODE_UNSOLICITED;
        lcmd->comm = LLC_COMM_MPICH;
        lcmd->rank = (uint32_t)raddr; /* XXX */
        lcmd->req_id = lcmd;
        
        lcmd->iov_local[0].addr = (uintptr_t)buff;
        lcmd->iov_local[0].length = bsiz;
        lcmd->niov_local = 1;
        
        lcmd->iov_remote[0].addr = 0;
        lcmd->iov_remote[0].length = bsiz;
        lcmd->niov_remote = 1;
        
        {
            struct llctofu_cmd_area *usr = (void *)lcmd->usr_area;
            usr->cbarg = cbarg;
            usr->raddr = lcmd->rank;
        }
        buff = 0;
    }

    {
        int iv, nv = niov;
        char *bp;
	size_t bz;

	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE, "llctofu_writev() : nv %d", nv);
	bp = (void *)lcmd->iov_local[0].addr;
	bz = lcmd->iov_local[0].length;

    /* Prepare netmod header */
    ((MPID_nem_tofu_netmod_hdr_t*)bp)->initiator_pg_rank = MPIDI_Process.my_pg_rank;
    bp += sizeof(MPID_nem_tofu_netmod_hdr_t);

    /* Pack iovs into buff */
	for (iv = 0; iv < nv; iv++) {
	    size_t len = iovs[iv].iov_len;
        
	    if (len <= 0) {
            continue;
	    }
	    if (len > bz) {
            len = bz;
	    }
	    memcpy(bp, iovs[iv].iov_base, len);
	    if ((bz -= len) <= 0) {
            break;
	    }
	    bp += len;
	}
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE, "llctofu_writev() : iv %d", iv);
	{
	void *bb = (void *)lcmd->iov_local[0].addr;
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "wptr       = %d", (int)(bp - (char *)bb));
	MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
	    "blocklengt = %d", (int)lcmd->iov_local[0].length);
	MPIU_DBG_PKT(endpt, bb, "writev");
	}
    }
    {
        int llc_errno;
        llc_errno = LLC_post(lcmd, 1);
        if (llc_errno != 0) {
            if ((llc_errno == EAGAIN) || (llc_errno == ENOSPC)) {
                nw = 0;
            }
            else {
                if (lcmd->iov_local[0].addr != 0) {
                    MPIU_Free((void *)lcmd->iov_local[0].addr);
                    lcmd->iov_local[0].addr = 0;
                }
                (void) LLC_cmd_free(lcmd, 1);
                nw = -1;
                goto bad;
            }
        }
        else {
            nw = (ssize_t)lcmd->iov_local[0].length;
        }
    }
    if (vpp_reqid != 0) {
        vpp_reqid[0] = lcmd;
    }
    
 bad:
    MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE, "llctofu_writev() : nw %d", (int)nw);
    return nw;
}

int llctofu_poll(int in_blocking_poll,
            llctofu_send_f sfnc, llctofu_recv_f rfnc)
{
    int mpi_errno = MPI_SUCCESS;

    int llc_errno;
    int nevents;
    LLC_event_t events[1];

    while(1) { 
        llc_errno = LLC_poll(LLC_COMM_MPICH, 1, events, &nevents);
        MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_poll");

        LLC_cmd_t *lcmd;
        void *vp_sreq;
        uint64_t reqid = 0;
        
        if(nevents == 0) {
            break;
        }
        MPIU_Assert(nevents == 1);
        
        switch(events[0].type) {
        case LLC_EVENT_SEND_LEFT: {
            dprintf("llctofu_poll,EVENT_SEND_LEFT\n");
            lcmd = events[0].side.initiator.req_id;
            MPIU_Assert(lcmd != 0);
            MPIU_Assert(lcmd->opcode == LLC_OPCODE_SEND);
            
            if(events[0].side.initiator.error_code != LLC_ERROR_SUCCESS) {
                printf("llctofu_poll,error_code=%d\n", events[0].side.initiator.error_code);
                MPID_nem_tofu_segv;
            }
 
            /* Call send_handler. First arg is a pointer to MPID_Request */
            (*sfnc)(((struct llctofu_cmd_area *)lcmd->usr_area)->cbarg, &reqid);
            
            /* Don't free iov_local[0].addr */

            llc_errno = LLC_cmd_free(lcmd, 1);
            MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_cmd_free");
            break; }

        case LLC_EVENT_UNSOLICITED_LEFT: {
            dprintf("llctofu_poll,EVENT_UNSOLICITED_LEFT\n");
            lcmd = events[0].side.initiator.req_id;
            MPIU_Assert(lcmd != 0);
            MPIU_Assert(lcmd->opcode == LLC_OPCODE_UNSOLICITED);
            
            struct llctofu_cmd_area *usr;
            usr = (void *)lcmd->usr_area;
            vp_sreq = usr->cbarg;
            
            UNSOLICITED_NUM_DEC(vp_sreq);

            if(events[0].side.initiator.error_code != LLC_ERROR_SUCCESS) {
                printf("llctofu_poll,error_code=%d\n", events[0].side.initiator.error_code);
                MPID_nem_tofu_segv;
            }
            (*sfnc)(vp_sreq, &reqid);
            
            if (lcmd->iov_local[0].addr != 0) {
                MPIU_Free((void *)lcmd->iov_local[0].addr);
                lcmd->iov_local[0].addr = 0;
            }
            llc_errno = LLC_cmd_free(lcmd, 1);
            MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_cmd_free");
            
            break; }
        case LLC_EVENT_UNSOLICITED_ARRIVED: {
            void *vp_vc = 0;
            uint64_t addr;
            void *buff;
            size_t bsiz;
            
            buff = events[0].side.responder.addr;
            bsiz = events[0].side.responder.length;
            {
                MPIU_DBG_MSG_D(CH3_CHANNEL, VERBOSE,
                               "LLC_leng   = %d", (int)bsiz);
                MPIU_DBG_PKT(vp_vc, buff, "poll");
            }
            dprintf("tofu_poll,EVENT_UNSOLICITED_ARRIVED,%d<-%d\n",
                    MPIDI_Process.my_pg_rank,
                    ((MPID_nem_tofu_netmod_hdr_t*)buff)->initiator_pg_rank
                    );
            (*rfnc)(vp_vc,
                    ((MPID_nem_tofu_netmod_hdr_t*)buff)->initiator_pg_rank,
                    (uint8_t*)buff + sizeof(MPID_nem_tofu_netmod_hdr_t),
                    bsiz);
            llc_errno = LLC_release_buffer(&events[0]);
            MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_release_buffer");
            
            break; }
        case LLC_EVENT_RECV_MATCHED: {
            dprintf("llctofu_poll,EVENT_RECV_MATCHED\n");
            lcmd = events[0].side.initiator.req_id;
            MPID_Request *req =  ((struct llctofu_cmd_area*)lcmd->usr_area)->cbarg;

            /* Unpack non-contiguous dt */
            int is_contig;
            MPID_Datatype_is_contig(req->dev.datatype, &is_contig);
            if (!is_contig) {
                dprintf("llctofu_poll,unpack noncontiguous data to user buffer\n");

                /* see MPIDI_CH3U_Request_unpack_uebuf (in /src/mpid/ch3/src/ch3u_request.c) */
                /* or MPIDI_CH3U_Receive_data_found (in src/mpid/ch3/src/ch3u_handle_recv_pkt.c) */
                MPIDI_msg_sz_t unpack_sz = req->dev.recv_data_sz;
                MPID_Segment seg;
                MPI_Aint last;

                /* user_buf etc. are set in MPID_irecv --> MPIDI_CH3U_Recvq_FDU_or_AEP */
                MPID_Segment_init(req->dev.user_buf, req->dev.user_count, req->dev.datatype, &seg,
                                  0);
                last = unpack_sz;
                MPID_Segment_unpack(&seg, 0, &last, REQ_FIELD(req, pack_buf));
                if (last != unpack_sz) {
                    /* --BEGIN ERROR HANDLING-- */
                    /* received data was not entirely consumed by unpack()
                     * because too few bytes remained to fill the next basic
                     * datatype */
                    MPIR_STATUS_SET_COUNT(req->status, last);
                    req->status.MPI_ERROR =
                        MPIR_Err_create_code(MPI_SUCCESS, MPIR_ERR_RECOVERABLE, FCNAME, __LINE__,
                                             MPI_ERR_TYPE, "**llctofu_poll", 0);
                    /* --END ERROR HANDLING-- */
                }
                dprintf("llctofu_poll,ref_count=%d,pack_buf=%p\n", req->ref_count,
                        REQ_FIELD(req, pack_buf));
                MPIU_Free(REQ_FIELD(req, pack_buf));
            }

            req->status.MPI_TAG = req->dev.match.parts.tag;
            req->status.MPI_SOURCE = req->dev.match.parts.rank;
            MPIR_STATUS_SET_COUNT(req->status, events[0].side.initiator.length);

            /* Dequeue request from posted queue.  
               It's posted in MPID_Irecv --> MPIDI_CH3U_Recvq_FDU_or_AEP */
            int found = MPIDI_CH3U_Recvq_DP(req);
            MPIU_Assert(found);

            /* Mark completion on rreq */
            MPIDI_CH3U_Request_complete(req);

            llc_errno = LLC_cmd_free(lcmd, 1);
            MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_cmd_free");
            break; }
        default:
            printf("llctofu_poll,unknown event type=%d\n", events[0].type);
            MPID_nem_tofu_segv;
        }
    }
    
 fn_exit:
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}
