/*! ----------------------------------------------------------------------------
*  @file    ss_resp_main.c
*  @brief   Single-sided two-way ranging (SS TWR) responder example code
*
*           This is a simple code example which acts as the responder in a SS TWR distance measurement exchange. 
*           This application waits for a "poll" message (recording the RX time-stamp of the poll) expected from 
*           the "SS TWR initiator" example code (companion to this application), and
*           then sends a "response" message recording its TX time-stamp.
*
*           Notes at the end of this file, to expand on the inline comments.
*
* @attention
*
* Copyright 2018 (c) Decawave Ltd, Dublin, Ireland.
*
* All rights reserved.
*
* @author Decawave
*/
#include "sdk_config.h" 
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "port_platform.h"
#include "init_resp_common.h"

/* Inter-ranging delay period, in milliseconds. See NOTE 1*/
#define RNG_DELAY_MS 80

static uint8 rx_poll_msg[POLL_MSG_LEN] = {0x41, 0x88, 0, 0xCA, 0xDE, 'W',
                                          'A',  0,    0, 0x61, 0,    0};
static uint8 tx_resp_msg[RESP_MSG_LEN] = {
    0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0x50,
    0,    0,    0, 0,    0,    0,   0,   0,   0,   0};
static uint8 rx_final_msg[FINAL_MSG_LEN] = {
    0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0x69, 0, 0,
    0,    0,    0, 0,    0,    0,   0,   0,   0,   0,    0, 0};
static uint8 tx_report_msg[REPORT_MSG_LEN] = {
    0x41, 0x88, 0, 0xCA, 0xDE, 'V', 'E', 'W', 'A', 0xE3, 0, 0, 0, 0, 0, 0};

/* Length of the common part of the message (up to and including the function code, see NOTE 3 below). */
#define ALL_MSG_COMMON_LEN 10

/* Index to access some of the fields in the frames involved in the process. */
#define ALL_MSG_SN_IDX 2
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14
#define RESP_MSG_TS_LEN 4	

/* Frame sequence number, incremented after each transmission. */
static uint8 frame_seq_nb = 0;

/* Buffer to store received response message.
* Its size is adjusted to longest frame that this example code is supposed to handle. */
#define RX_BUF_LEN 24
static uint8 rx_buffer[RX_BUF_LEN];

/* Hold copy of status register state here for reference so that it can be examined at a debug breakpoint. */
static uint32 status_reg = 0;

/* UWB microsecond (uus) to device time unit (dtu, around 15.65 ps) conversion factor.
* 1 uus = 512 / 499.2 µs and 1 µs = 499.2 * 128 dtu. */
#define UUS_TO_DWT_TIME 65536

// Not enough time to write the data so TX timeout extended for nRF operation.
// Might be able to get away with 800 uSec but would have to test
// See note 6 at the end of this file
#define POLL_RX_TO_RESP_TX_DLY_UUS 1500
#define RX_BUF_LEN MAX(POLL_MSG_LEN, FINAL_MSG_LEN)

static int wait_poll_message(uint16_t *src_id, uint32_t *logic_clk) {
    uint32 status_reg, frame_len;

    dwt_rxenable(DWT_START_RX_IMMEDIATE);

    UWB_WAIT(
        (status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
        (SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR)) {

    }

    if (!(status_reg & SYS_STATUS_RXFCG)) {
        dwt_write32bitreg(SYS_STATUS_ID,
                          SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_rxreset();
        return -1;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG);

    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFL_MASK_1023;
    if (frame_len <= RX_BUFFER_LEN) {
        dwt_readrxdata(rx_buffer, frame_len, 0);
    }

    rx_buffer[SEQ_CNT_OFFSET] = 0;
    *src_id = get_src_id(rx_buffer);
    rx_buffer[SRC_OFFSET] = 0;
    rx_buffer[SRC_OFFSET + 1] = 0;
    GET_EXCHANGE_ID(rx_buffer + LOGIC_CLK_OFFSET, *logic_clk);
    SET_EXCHANGE_ID(rx_buffer + LOGIC_CLK_OFFSET, 0);
    if (!(memcmp(rx_buffer, rx_poll_msg, DW_BASE_LEN) == 0)) {
        return -1;
    }

    return 0;
}


static int ds_respond(uint64_t *poll_rx_ts) {
    uint32 resp_tx_time;
    int ret;

    *poll_rx_ts = get_rx_timestamp_u64();

    resp_tx_time =
        (*poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
    dwt_setdelayedtrxtime(resp_tx_time);

    dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
    dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);

    ret = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);

    if (ret != DWT_SUCCESS) {
        dwt_rxreset();
        return -1;
    }

    UWB_WAIT(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS);
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);

    return 0;
}



static int wait_final(uint64 *tof_dtu, const uint64_t *poll_rx_ts) {
    uint32 status_reg, frame_len, poll_rx_ts_32, resp_tx_ts_32, final_rx_ts_32;
    uint32_t resp_rx_ts, poll_tx_ts, final_tx_ts;
    uint64_t final_rx_ts, resp_tx_ts;
    double roundA, replyA, roundB, replyB;

    UWB_WAIT(
        (status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
        (SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR)) {

    }

    if (!(status_reg & SYS_STATUS_RXFCG)) {
        dwt_write32bitreg(SYS_STATUS_ID,
                          SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_rxreset();
        return -1;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG);

    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFL_MASK_1023;
    if (frame_len <= RX_BUFFER_LEN) {
        dwt_readrxdata(rx_buffer, frame_len, 0);
    }

    rx_buffer[SEQ_CNT_OFFSET] = 0;

    if (!(memcmp(rx_buffer, rx_final_msg, DW_BASE_LEN) == 0)) {
        return -1;
    }

    final_rx_ts = get_rx_timestamp_u64();
    resp_tx_ts = get_tx_timestamp_u64();

    msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &poll_tx_ts);
    msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_rx_ts);
    msg_get_ts(&rx_buffer[FINAL_MSG_FINAL_TX_TS_IDX], &final_tx_ts);

    poll_rx_ts_32 = (uint32)(*poll_rx_ts);
    resp_tx_ts_32 = (uint32)resp_tx_ts;
    final_rx_ts_32 = (uint32)final_rx_ts;
    roundB = (double)(final_rx_ts_32 - resp_tx_ts_32);
    replyB = (double)(resp_tx_ts_32 - poll_rx_ts_32);
    roundA = (double)(resp_rx_ts - poll_tx_ts);
    replyA = (double)(final_tx_ts - resp_rx_ts);

    if ((roundA * roundB - replyA * replyB) <= 0) {
        return -3;
    }

    *tof_dtu = (uint64)((roundA * roundB - replyA * replyB) /
                        (roundA + roundB + replyA + replyB));

    return 0;
}


static int send_report(uint64 tof_dtu) {
    int ret;

    msg_set_ts(&tx_report_msg[RESP_MSG_POLL_RX_TS_IDX], tof_dtu);

    dwt_writetxdata(sizeof(tx_report_msg), tx_report_msg, 0);
    dwt_writetxfctrl(sizeof(tx_report_msg), 0, 1);
    ret = dwt_starttx(DWT_START_TX_IMMEDIATE);

    if (ret != DWT_SUCCESS) {
        dwt_rxreset();
        return -1;
    }

    UWB_WAIT(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS);
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);

    return 0;
}


int ds_resp_run() {
    int err;
    uint16_t src_id = 1;
    uint16_t id = 2;
    uint64 tof_dtu;
    uint64_t poll_rx_ts;
    uint32_t _logic_clk = 0;


    if ((err = wait_poll_message(&src_id, &_logic_clk)) < 0) {
        return err;
    }

    set_dest_id(src_id, tx_resp_msg);
    SET_EXCHANGE_ID(tx_resp_msg + LOGIC_CLK_OFFSET, _logic_clk);

    if ((err = ds_respond(&poll_rx_ts)) < 0) {
        return err;
    }

    set_src_id(src_id, rx_final_msg);
    SET_EXCHANGE_ID(rx_final_msg + LOGIC_CLK_OFFSET, _logic_clk);

    if ((err = wait_final(&tof_dtu, &poll_rx_ts)) < 0) {
        return err;
    }

    set_dest_id(src_id, tx_report_msg);
    SET_EXCHANGE_ID(tx_report_msg + LOGIC_CLK_OFFSET, _logic_clk);

    if ((err = send_report(tof_dtu)) < 0) {
        return err;
    }

    return 0;
}


/*! ------------------------------------------------------------------------------------------------------------------
* @fn final_msg_set_ts()
*
* @brief Fill a given timestamp field in the response message with the given value. In the timestamp fields of the
*        response message, the least significant byte is at the lower address.
*
* @param  ts_field  pointer on the first byte of the timestamp field to fill
*         ts  timestamp value
*
* @return none
*/
static void resp_msg_set_ts(uint8 *ts_field, const uint64 ts)
{
  int i;
  for (i = 0; i < RESP_MSG_TS_LEN; i++)
  {
    ts_field[i] = (ts >> (i * 8)) & 0xFF;
  }
}


/**@brief SS TWR Initiator task entry function.
*
* @param[in] pvParameter   Pointer that will be used as the parameter for the task.
*/
void ss_responder_task_function (void * pvParameter)
{
  UNUSED_PARAMETER(pvParameter);

  dwt_setleds(DWT_LEDS_ENABLE);

  while (true)
  {
    ds_resp_run();
    /* Delay a task for a given number of ticks */
    vTaskDelay(RNG_DELAY_MS);
    /* Tasks must be implemented to never return... */
  }
}