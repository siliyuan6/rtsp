#ifndef RTP_H
#define RTP_H

#include <stdint.h>

typedef struct FU_INDICATOR_S {
    uint8_t u5Type : 5;
    uint8_t u2Nri : 2;
    uint8_t u1F : 1;
} FU_INDICATOR_T;

typedef struct FU_HEADER_S {
   	uint8_t u5Type : 5;
    uint8_t u1R : 1;
    uint8_t u1E : 1;
    uint8_t u1S : 1;
} FU_HEADER_T;

typedef struct RTP_FIXED_HEADER_S {
    uint8_t u4CSrcLen : 4;    /* expect 0 */
    uint8_t u1Externsion : 1; /* expect 1, see RTP_OP below */
    uint8_t u1Padding : 1;    /* expect 0 */
    uint8_t u2Version : 2;    /* expect 2 */

    uint8_t u7Payload : 7; /* RTP_PAYLOAD_RTSP */
    uint8_t u1Marker : 1;  /* expect 1 */

    uint16_t u16SeqNum;

    uint32_t u32TimeStamp;

    uint32_t u32SSrc; /* stream number is used here. */
} RTP_FIXED_HEADER_T;

typedef struct NALU_HEADER_S {
    uint8_t u5Type : 5;
    uint8_t u2Nri : 2;
    uint8_t u1F : 1;
} NALU_HEADER_T;

typedef enum {
	NALU_SIGNEL = 0,     // NALU packet with only one NALU
	NALU_MUTIL,          // NALU packet with multiple NALU
	NALU_SLICE           // NALU packet with slice header
} NALU_E;

typedef enum {
    FRAME_NOIDR = 1,   // NO-IDR
    FRAME_PART_A,      // Part A
    FRAME_PART_B,      // Part B
    FRAME_PART_C,      // Part C
    FRAME_IDR   = 5,   // IDR
    FRAME_SEI,         // SEI
    FRAME_SPS,         // SPS
    FRAME_PPS,         // PPS
    FRAME_ACCESS,      // Access Unit
    FRAME_END   = 10,  // End Unit

    FRAME_FU_A  = 28,  // FU-A Unit
    FRAME_FU_B  = 29,  // FU-B Unit
} FRAME_E;

typedef struct {
    int rtp_wsa_flag;               // flag for Winsock initialization
    int rtp_listen_port[2];         // port for RTP communication
    int rtp_fd[2];                  // file descriptor for RTP communication
                                    // 0: RTP; 1: RTCP
    void *rtp_recv_buf;             // buffer for receiving RTP packet
    int rtp_recv_len;               // length of received RTP packet

    FU_INDICATOR_T     fu_ind;      // FU indicator
    FU_HEADER_T        fu_hdr;      // FU header
    RTP_FIXED_HEADER_T rtp_hdr;     // RTP fixed header
    NALU_HEADER_T      nalu_hdr;    // NALU header
    NALU_E nalu_type;               // NALU type

    FILE *fd;                       // file descriptor for saving stream
    char stream_filename[64];       // filename for saving stream
    int last_frame_type;            // last frame type
    int last_last_frame_type;       // last last frame type

} rtp_t;

int rtp_create(void **ctx);
int rtp_destroy(void *ctx);
int rtp_pkg_parse(void *ctx, void *buffer, int len);
int rtcp_pkg_parse(void *buffer, int len);

#endif /* RTP_H */