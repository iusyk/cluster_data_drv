#ifndef R_TAURUS_CLUSTER_PROTOCOL_H
#define R_TAURUS_CLUSTER_PROTOCOL_H

#ifndef __packed
# define __packed       __attribute__((__packed__))
#endif

#include "r_taurus_bridge.h"

typedef struct taurus_cluster_data {
    int   speed;
    int   rpm
}taurus_cluster_data_t;

typedef struct taurus_cluster_res_msg {
    R_TAURUS_ResultMsg_t hdr;
}taurus_cluster_res_msg_t;


#endif /* R_TAURUS_CLUSTER_PROTOCOL_H */
