/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/* vim: set ts=8 sts=4 sw=4 noexpandtab : */
/*
 *
 */



#include "mpid_nem_impl.h"
#include "tofu_impl.h"

//#define MPID_NEM_TOFU_DEBUG_INIT
#ifdef MPID_NEM_TOFU_DEBUG_INIT
#define dprintf printf
#else
#define dprintf(...)
#endif

/* global variables */

/* src/mpid/ch3/channels/nemesis/include/mpid_nem_nets.h */

MPID_nem_netmod_funcs_t MPIDI_nem_tofu_funcs = {
    .init		= MPID_nem_tofu_init,
    .finalize		= MPID_nem_tofu_finalize,
#ifdef	ENABLE_CHECKPOINTING
    .ckpt_precheck	= NULL,
    .ckpt_restart	= NULL,
    .ckpt_continue	= NULL,
#endif
    .poll		= MPID_nem_tofu_poll,
    .get_business_card	= MPID_nem_tofu_get_business_card,
    .connect_to_root	= MPID_nem_tofu_connect_to_root,
    .vc_init		= MPID_nem_tofu_vc_init,
    .vc_destroy		= MPID_nem_tofu_vc_destroy,
    .vc_terminate	= MPID_nem_tofu_vc_terminate,
    .anysource_iprobe	= MPID_nem_tofu_anysource_iprobe,
    .anysource_improbe	= MPID_nem_tofu_anysource_improbe,
};

int MPID_nem_tofu_my_llc_rank;

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_kvs_put_binary
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_kvs_put_binary(int from, const char *postfix, const uint8_t * buf,
                                      int length)
{
    int mpi_errno = MPI_SUCCESS;
    int pmi_errno;
    char *kvs_name;
    char key[256], val[256], str[256];
    int j;

    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_KVS_PUT_BINARY);
    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_KVS_PUT_BINARY);

    mpi_errno = MPIDI_PG_GetConnKVSname(&kvs_name);
    MPIU_ERR_CHKANDJUMP(mpi_errno, mpi_errno, MPI_ERR_OTHER, "**MPIDI_PG_GetConnKVSname");
    dprintf("kvs_put_binary,kvs_name=%s\n", kvs_name);

    sprintf(key, "bc/%d/%s", from, postfix);
    val[0] = 0;
    for (j = 0; j < length; j++) {
        sprintf(str, "%02x", buf[j]);
        strcat(val, str);
    }
    dprintf("kvs_put_binary,rank=%d,from=%d,PMI_KVS_Put(%s, %s, %s)\n",
            MPIDI_Process.my_pg_rank, from, kvs_name, key, val);
    pmi_errno = PMI_KVS_Put(kvs_name, key, val);
    MPIU_ERR_CHKANDJUMP(pmi_errno, mpi_errno, MPI_ERR_OTHER, "**PMI_KVS_Put");
  fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_KVS_PUT_BINARY);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_kvs_get_binary
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int MPID_nem_tofu_kvs_get_binary(int from, const char *postfix, char *buf, int length)
{
    int mpi_errno = MPI_SUCCESS;
    int pmi_errno;
    char *kvs_name;
    char key[256], val[256], str[256];
    int j;

    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_KVS_GET_BINARY);
    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_KVS_GET_BINARY);

    mpi_errno = MPIDI_PG_GetConnKVSname(&kvs_name);
    dprintf("kvs_get_binary,kvs_name=%s\n", kvs_name);
    MPIU_ERR_CHKANDJUMP(mpi_errno, mpi_errno, MPI_ERR_OTHER, "**MPIDI_PG_GetConnKVSname");

    sprintf(key, "bc/%d/%s", from, postfix);
    dprintf("kvs_put_binary,rank=%d,from=%d,PMI_KVS_Get(%s, %s, %s)\n",
            MPIDI_Process.my_pg_rank, from, kvs_name, key, val);
    pmi_errno = PMI_KVS_Get(kvs_name, key, val, 256);
    MPIU_ERR_CHKANDJUMP(pmi_errno, mpi_errno, MPI_ERR_OTHER, "**PMS_KVS_Get");

    dprintf("rank=%d,obtained val=%s\n", MPIDI_Process.my_pg_rank, val);
    char *strp = val;
    for (j = 0; j < length; j++) {
        memcpy(str, strp, 2);
        str[2] = 0;
        buf[j] = strtol(str, NULL, 16);
        strp += 2;
    }

  fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_KVS_GET_BINARY);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_init
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
MPID_nem_tofu_init (MPIDI_PG_t *pg_p, int pg_rank,
		  char **bc_val_p, int *val_max_sz_p)
{
    int mpi_errno = MPI_SUCCESS, pmi_errno, llc_errno;
    int rc;
    int i;
    int llc_rank;

    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_INIT);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_INIT);

    llc_errno = LLC_init(TYPE_MPI);
    MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_init");

    llc_errno = LLC_comm_rank(LLC_COMM_MPICH, &MPID_nem_tofu_my_llc_rank);
    MPIU_ERR_CHKANDJUMP(llc_errno, mpi_errno, MPI_ERR_OTHER, "**LLC_comm_rank");

    /* Announce my LLC rank */
    mpi_errno =
        MPID_nem_tofu_kvs_put_binary(pg_rank, "llc_rank",
                                     (uint8_t *) & MPID_nem_tofu_my_llc_rank,
                                     sizeof(int));
    MPIU_ERR_CHKANDJUMP(mpi_errno, mpi_errno, MPI_ERR_OTHER,
                        "**MPID_nem_ib_kvs_put_binary");
    dprintf("tofu_init,my_pg_rank=%d,my_llc_rank=%d\n",
            MPIDI_Process.my_pg_rank, MPID_nem_tofu_my_llc_rank);

    /* Wait until the key-value propagates among all ranks */
    pmi_errno = PMI_Barrier();
    MPIU_ERR_CHKANDJUMP(pmi_errno != PMI_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**PMI_Barrier");

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_INIT);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_get_business_card
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
MPID_nem_tofu_get_business_card (int my_rank, char **bc_val_p, int *val_max_sz_p)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_GET_BUSINESS_CARD);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_GET_BUSINESS_CARD);

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_GET_BUSINESS_CARD);
    return mpi_errno;
    //fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_connect_to_root
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
MPID_nem_tofu_connect_to_root (const char *business_card, MPIDI_VC_t *new_vc)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_TOFU_CONNECT_TO_ROOT);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_TOFU_CONNECT_TO_ROOT);

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_TOFU_CONNECT_TO_ROOT);
    return mpi_errno;
    //fn_fail:
    goto fn_exit;
}

/* ============================================== */
/* ================ tofu_probe.c ================ */
/* ============================================== */

#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_anysource_iprobe
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPID_nem_tofu_anysource_iprobe(int tag, MPID_Comm *comm, int context_offset, int *flag, MPI_Status *status)
{
    return MPI_SUCCESS;
}


#undef FUNCNAME
#define FUNCNAME MPID_nem_tofu_anysource_improbe
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPID_nem_tofu_anysource_improbe(int tag, MPID_Comm *comm, int context_offset, int *flag, MPID_Request **message, MPI_Status *status)
{
    return MPI_SUCCESS;
}
