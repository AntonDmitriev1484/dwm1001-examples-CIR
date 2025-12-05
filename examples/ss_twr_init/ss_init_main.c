/*! ----------------------------------------------------------------------------
*  @file    ss_init_main.c
*  @brief   Single-sided two-way ranging (SS TWR) initiator example code
*
*           This is a simple code example which acts as the initiator in a SS TWR distance measurement exchange. This application sends a "poll"
*           frame (recording the TX time-stamp of the poll), after which it waits for a "response" message from the "DS TWR responder" example
*           code (companion to this application) to complete the exchange. The response message contains the remote responder's time-stamps of poll
*           RX, and response TX. With this data and the local time-stamps, (of poll TX and response RX), this example application works out a value
*           for the time-of-flight over-the-air and, thus, the estimated distance between the two devices, which it writes to the LCD.
*
*
*           Notes at the end of this file, expand on the inline comments.
* 
* @attention
*
* Copyright 2015 (c) Decawave Ltd, Dublin, Ireland.
*
* All rights reserved.
*
* @author Decawave
*/
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "port_platform.h"
#include "deca_device_api.h"

#include "init_resp_common.h"
#include "SEGGER_RTT.h"

#define APP_NAME "SS TWR INIT v1.3"


#define N_CIR_SAMPLES 128

typedef struct {
    int16 real;  // Use int16 (DecaDriver style) not int16_t
    int16 imag;
}  cir_sample_t;

typedef struct {
    float range;
    dwt_rxdiag_t diagnostics;
    cir_sample_t cir_samples[N_CIR_SAMPLES] __attribute__((aligned(4)));
} __attribute__((packed)) range_packet_t;

/* Inter-ranging delay period, in milliseconds. */
#define RNG_DELAY_MS 100

/* Frames used in the ranging process. See NOTE 1,2 below. */
static uint8 tx_poll_msg[POLL_MSG_LEN] = {0x41, 0x88, 0,   0xCA, 0xDE, 'W',
                                          'A',  'V',  'E', 0x61, 0,    0};
static uint8 rx_resp_msg[RESP_MSG_LEN] = {
    0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0x50,
    0,    0,    0, 0,    0,    0,   0,   0,   0,   0};
static uint8 tx_final_msg[FINAL_MSG_LEN] = {
    0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0x69, 0, 0,
    0,    0,    0, 0,    0,    0,   0,   0,   0,   0,    0, 0};
static uint8 rx_report_msg[REPORT_MSG_LEN] = {
    0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0xE3, 0, 0, 0, 0, 0, 0};


#define RESP_MSG_TS_LEN 4
/* Frame sequence number, incremented after each transmission. */
static uint8 frame_seq_nb = 0;

/* Buffer to store received response message.
* Its size is adjusted to longest frame that this example code is supposed to handle. */
#define RX_BUF_LEN FINAL_MSG_LEN
static uint8 rx_buffer[RX_BUF_LEN];

/* Hold copy of status register state here for reference so that it can be examined at a debug breakpoint. */
static uint32 status_reg = 0;

/* UWB microsecond (uus) to device time unit (dtu, around 15.65 ps) conversion factor.
* 1 uus = 512 / 499.2 µs and 1 µs = 499.2 * 128 dtu. */
#define UUS_TO_DWT_TIME 65536

/* Speed of light in air, in metres per second. */
#define SPEED_OF_LIGHT 299702547

/* Hold copies of computed time of flight and distance here for reference so that it can be examined at a debug breakpoint. */
static double tof;
static double distance;

/*Transactions Counters */
static volatile int tx_count = 0 ; // Successful transmit counter
static volatile int rx_count = 0 ; // Successful receive counter 

#define POLL_RX_TO_RESP_TX_DLY_UUS 2000
#define POLL_TX_TO_RESP_RX_DLY_UUS 300

static int send_poll(void) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);
    dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
    dwt_writetxfctrl(sizeof(tx_poll_msg), 0, 1);

    int check_poll_msg =
        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

    if (check_poll_msg != DWT_SUCCESS) {
        return -1;
    }

    UWB_WAIT(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS);
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);

    return 0;
}

static int ds_rx_response(void) {
    uint32 status_reg, frame_len;

    UWB_WAIT((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
             (SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR));

    if (!(status_reg & SYS_STATUS_RXFCG)) {
        dwt_write32bitreg(SYS_STATUS_ID,
                          SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_rxreset();
        return -1;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG);

    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFLEN_MASK;

    if (frame_len <= RX_BUF_LEN) {
        dwt_readrxdata(rx_buffer, frame_len, 0);
    }

    rx_buffer[SEQ_CNT_OFFSET] = 0;

    if (!(memcmp(rx_buffer, rx_resp_msg, DW_BASE_LEN) == 0)) {
        return -1;
    }

    return 0;
}

static int send_final(void) {
    uint64 poll_tx_ts, resp_rx_ts;
    uint64 ts_replyA_end;
    uint32 resp_tx_time;
    int ret;

    poll_tx_ts = get_tx_timestamp_u64();
    resp_rx_ts = get_rx_timestamp_u64();

    resp_tx_time =
        (resp_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
    dwt_setdelayedtrxtime(resp_tx_time);

    ts_replyA_end = (((uint64)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

    msg_set_ts(&tx_final_msg[RESP_MSG_POLL_RX_TS_IDX], poll_tx_ts);
    msg_set_ts(&tx_final_msg[RESP_MSG_RESP_TX_TS_IDX], resp_rx_ts);
    msg_set_ts(&tx_final_msg[FINAL_MSG_FINAL_TX_TS_IDX], ts_replyA_end);

    dwt_writetxdata(sizeof(tx_final_msg), tx_final_msg, 0);
    dwt_writetxfctrl(sizeof(tx_final_msg), 0, 1);

    ret = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);

    if (ret != DWT_SUCCESS) {
        dwt_rxreset();
        return -2;
    }

    UWB_WAIT(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS);
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);

    return 0;
}

static int rx_report(double *distance, cir_sample_t* cir_samples, dwt_rxdiag_t* diag) {
    uint32 status_reg, frame_len;
    uint32_t msg_tof_dtu;
    double tof;

    UWB_WAIT((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
             (SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR));

    if (!(status_reg & SYS_STATUS_RXFCG)) {
        dwt_write32bitreg(SYS_STATUS_ID,
                          SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_rxreset();
        return -1;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG);

    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFLEN_MASK;
    if (frame_len > RX_BUFFER_LEN) {
        dwt_rxreset();
        return -2;
    }

    dwt_readrxdata(rx_buffer, frame_len, 0);

    rx_buffer[SEQ_CNT_OFFSET] = 0;

    if (memcmp(rx_buffer, rx_report_msg, DW_BASE_LEN) != 0) {
        return -3;
    }

    // Compute ToF
    msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &msg_tof_dtu);
    tof = msg_tof_dtu * DWT_TIME_UNITS;
    *distance = tof * SPEED_OF_LIGHT;

    // Diagnostics
    dwt_readdiagnostics(diag);

    // CIR
    uint16 fp_index = diag->firstPath >> 6;
    int start = fp_index - (N_CIR_SAMPLES / 2);
    if (start < 0) start = 0;
    if (start + N_CIR_SAMPLES > 1024) start = 1024 - N_CIR_SAMPLES;

    uint16 accOffset = start * sizeof(cir_sample_t);
    uint16 N_bytes = N_CIR_SAMPLES * sizeof(cir_sample_t);

    dwt_readaccdata((uint8_t*)cir_samples, N_bytes, accOffset);

    return 0;
}



static range_packet_t packet; // Allocating this on stack will create hardfault!!!

int ds_init_run(void)
{
    // Getting mem fault somewhere in here
    int err;

    if ((err = send_poll()) < 0) {
        return err;
    }
    if ((err = ds_rx_response()) < 0) {
        return err;
    }

    if ((err = send_final()) < 0) {
        return err;
    }

    
    double distance;
    dwt_rxdiag_t decawave_diag;
    //cir_sample_t samples[N_CIR_SAMPLES];
    if ((err = rx_report(&distance, packet.cir_samples, &decawave_diag)) < 0) {
        return err;
    } // Actually seems to make it through this entire function

    printf("d=%f",distance);

    
    packet.range = distance;
    packet.diagnostics = decawave_diag;
    //memcpy(packet.cir_samples, samples, N_CIR_SAMPLES *sizeof(cir_sample_t));

    //printf("packet size %d", sizeof(range_packet_t));

    //SEGGER_RTT_Write(0, (uint8_t*)&packet, sizeof(packet));

    return 0;
}



/*! ------------------------------------------------------------------------------------------------------------------
* @fn resp_msg_get_ts()
*
* @brief Read a given timestamp value from the response message. In the timestamp fields of the response message, the
*        least significant byte is at the lower address.
*
* @param  ts_field  pointer on the first byte of the timestamp field to get
*         ts  timestamp value
*
* @return none
*/
static void resp_msg_get_ts(uint8 *ts_field, uint32 *ts)
{
  int i;
  *ts = 0;
  for (i = 0; i < RESP_MSG_TS_LEN; i++)
  {
    *ts += ts_field[i] << (i * 8);
  }
}


/**@brief SS TWR Initiator task entry function.
*
* @param[in] pvParameter   Pointer that will be used as the parameter for the task.
*/
void ss_initiator_task_function (void * pvParameter)
{
  UNUSED_PARAMETER(pvParameter);

  //dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);

  dwt_setleds(DWT_LEDS_ENABLE);

  SEGGER_RTT_WriteString(0, "Hello from DWM1001!\n");
  printf("In ss_init_task_func");

  // Can do
  //  int32_t values[5] = {1, 2, 3, 4, 5};
  //SEGGER_RTT_Write(0, (uint8_t*)values, sizeof(values));

  //Can also send structured packets (see GPT)

  while (true)
  {
    //ss_init_run();
    ds_init_run();
    /* Delay a task for a given number of ticks */
    vTaskDelay(RNG_DELAY_MS);
    /* Tasks must be implemented to never return... */
  }
}
