#ifndef R_TAURUS_CLUSTER_PROTOCOL_H
#define R_TAURUS_CLUSTER_PROTOCOL_H

#ifndef __packed
# define __packed       __attribute__((__packed__))
#endif

typedef struct {
    float   speed;
    int     rpm
}taurus_cluster_data_t;


#endif /* R_TAURUS_CLUSTER_PROTOCOL_H */
