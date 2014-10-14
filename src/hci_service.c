/******************************************************************************
 *
 *  Copyright (C) Intel 2014
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  Filename:   hci_service.c
 *
 *  Description:    HCI service gateway to offer native bindable service
 *      that can be accessed through Binder
 *
 ******************************************************************************/

#define LOG_TAG "bt_bind_service"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <utils/Log.h>

#include "libbtcellcoex-client.h"
#include "bt_hci_bdroid.h"
#include "bt_vendor_brcm.h"
#include "hardware/bluetooth.h"

/******************************************************************************
**  Constants & Macros
******************************************************************************/
#define BTHCISERVICE_DBG TRUE

#ifndef BTHCISERVICE_DBG
#define BTHCISERVICE_DBG FALSE
#endif

#ifndef BTHCISERVICE_VERB
#define BTHCISERVICE_VERB FALSE
#endif

#if (BTHCISERVICE_DBG == TRUE)
#define BTHSDBG(param, ...) {ALOGD(param, ## __VA_ARGS__);}
#else
#define BTHSDBG(param, ...) {}
#endif

#if (BTHCISERVICE_VERB == TRUE)
#define BTHSVERB(param, ...) {ALOGD(param, ## __VA_ARGS__);}
#else
#define BTHSVERB(param, ...) {}
#endif

/* Duplicated definitions from ./hardware.h */
#define HCI_EVT_CMD_CMPL_STATUS_RET_BYTE        5
#define HCI_EVT_CMD_CMPL_OPCODE                 3

#define STREAM_TO_UINT8(u8, p) {u8 = (uint8_t)(*(p)); (p) += 1;}
#define STREAM_TO_UINT16(u16, p) {u16 = ((uint16_t)(*(p)) + (((uint16_t)(*((p) + 1))) << 8)); (p) += 2;}

// Time to wait the HCI command complete event after that HCI command is sent.
// CAUTION: Must be < 1000. On stress tests, 60ms has been measured between
// the hci_cmd_send and the hci_cmd_cback calls.
#define WAIT_TIME_MS       500

/******************************************************************************
**  Static Variables
******************************************************************************/
static uint8_t status = -1;
static pthread_cond_t thread_cond;
static HC_BT_HDR *p_msg = NULL;
static pthread_mutex_t mutex;
static bool predicate = false;
static int bind_state = BTCELLCOEX_STATUS_NO_INIT;

/******************************************************************************
**  Functions
******************************************************************************/
static int hci_cmd_send(const size_t cmdLen, const void* cmdBuf);
void hci_bind_client_cleanup(void);
static void print_xmit(HC_BT_HDR *p_msg);

/*******************************************************************************
**
** Function         hci_bind_client_init
**
** Description     Initialization of the client, the signal to be able to
** send HCI commands from the bound interface.
**
** Returns          None
**
*******************************************************************************/
void hci_bind_client_init(void)
{
    int ret;

    BTHSVERB("%s enter", __FUNCTION__);

    bind_state = bindToCoexService(&hci_cmd_send);
    if(bind_state != BTCELLCOEX_STATUS_OK) {
        ALOGE("%s: bindToCoexService failure", __FUNCTION__);
        goto fail;
    }

    if ((ret = pthread_cond_init(&thread_cond, NULL)) != 0) {
        ALOGE("%s: pthread_cond_init failed: %s", __FUNCTION__, strerror(ret));
        goto fail;
    }
    if ((ret = pthread_mutex_init(&mutex, NULL)) != 0) {
        ALOGE("%s: pthread_mutex_init failed: %s", __FUNCTION__, strerror(ret));
        goto fail;
    }
    BTHSVERB("%s exit", __FUNCTION__);
    return;
fail:
    hci_bind_client_cleanup();
    return;
}

/*******************************************************************************
**
** Function         hci_bind_client_cleanup
**
** Description      Function called to clean ressources
**
** Returns          None
**
*******************************************************************************/
void hci_bind_client_cleanup(void)
{
    int ret = -1;
    BTHSDBG("%s", __FUNCTION__);

    if ((ret = pthread_mutex_destroy(&mutex)) != 0)
        ALOGE("%s: pthread_mutex_destroy failed: %s", __FUNCTION__, strerror(ret));
    if ((ret = pthread_cond_destroy(&thread_cond)) != 0)
        ALOGE("%s: pthread_cond_destroy failed: %s", __FUNCTION__, strerror(ret));

    return;
}

/*******************************************************************************
**
** Function         hci_cmd_cback
**
** Description     Callback invoked on completion of the HCI command
**
** Returns          None
**
*******************************************************************************/
static void hci_cmd_cback(void *p_mem)
{
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
    uint8_t     *p;
    uint16_t    opcode;
    int ret = -1;

    // Get the HCI command complete event status
    status = *((uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_STATUS_RET_BYTE);
    p = (uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_OPCODE;
    STREAM_TO_UINT16(opcode, p);
    if (status == 0) {
        BTHSDBG("%s: HCI with opcode: 0x%04X success", __FUNCTION__, opcode);
    } else {
        ALOGE("%s: HCI with opcode: 0x%04X failure", __FUNCTION__, opcode);
    }

    // For libbt internal commands, we need to deallocate the received buffer
    if (bt_vendor_cbacks)
        bt_vendor_cbacks->dealloc(p_evt_buf);

    if ((ret = pthread_mutex_lock(&mutex)) != 0) {
        ALOGE("%s: pthread_mutex_lock failed: %s", __FUNCTION__, strerror(ret));
        return;
    }
    predicate = true;
    // Wakeup the socket server thread so it can send the status to the sender
    if ((ret = pthread_cond_signal(&thread_cond)) != 0)
        ALOGE("%s: pthread_cond_signal failed: %s", __FUNCTION__, strerror(ret));
    if ((ret = pthread_mutex_unlock(&mutex)) != 0) {
        ALOGE("%s: pthread_mutex_unlock failed: %s", __FUNCTION__, strerror(ret));
        return;
    }

    return;
}

/*******************************************************************************
**
** Function         hci_cmd_send
**
*******************************************************************************/
int hci_cmd_send(const size_t cmdLen, const void* cmdBuf)
{
    uint8_t *p;
    struct timespec timeToWait;
    struct timeval currentTime;
    int ret = 0;
    uint8_t *pcmdBuf = (uint8_t *)cmdBuf;

    if(NULL == cmdBuf) {
        ALOGE("%s: null cmd pointer passed!", __FUNCTION__);
        return (BTCELLCOEX_STATUS_BAD_VALUE);
    }

    uint16_t opcode = (uint16_t)(*(pcmdBuf)) + (((uint16_t)(*((pcmdBuf) + 1))) << 8);
    uint8_t length = cmdLen;

    BTHSDBG("%s", __FUNCTION__);

    if (!bt_vendor_cbacks) {
        ALOGE("%s: bt_vendor_cbacks not initialized.", __FUNCTION__);
        return (BTCELLCOEX_STATUS_UNKNOWN_ERROR);
    }
    // For libbt internal commands, buffers are automatically deallocated
    if ((p_msg = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + length)) == NULL) {
        ALOGE("%s: failed to allocate buffer.", __FUNCTION__);
        return (BTCELLCOEX_STATUS_UNKNOWN_ERROR);
    }

    p_msg->event = MSG_STACK_TO_HC_HCI_CMD;
    p_msg->len = length;
    p_msg->offset = 0;

    p = (uint8_t *)(p_msg + 1);
    memcpy(p, cmdBuf, length);

    // Only if BTHCISERVICE_VERB == TRUE
    print_xmit(p_msg);

    if (bt_vendor_cbacks->xmit_cb(opcode, p_msg, hci_cmd_cback) == FALSE) {
        ALOGE("%s: failed to xmit buffer.", __FUNCTION__);
        bt_vendor_cbacks->dealloc(p_msg);
        return BTCELLCOEX_STATUS_UNKNOWN_ERROR;
    }

    gettimeofday(&currentTime, NULL);
    currentTime.tv_usec += 1000UL * WAIT_TIME_MS;
    if (currentTime.tv_usec >= 1000000UL) {
        ++currentTime.tv_sec;
        currentTime.tv_usec -= 1000000UL;
    }
    timeToWait.tv_sec = currentTime.tv_sec;
    timeToWait.tv_nsec = (currentTime.tv_usec + 1000UL * WAIT_TIME_MS) * 1000UL;

    ret = 0;
    while (!predicate && ret == 0)
        ret = pthread_cond_timedwait(&thread_cond, &mutex, &timeToWait);
    if (ret == 0) {
        BTHSVERB("%s: pthread_cond_timedwait succeed", __FUNCTION__);
        predicate = false;
    } else {
        pthread_mutex_unlock(&mutex);
        ALOGE("%s: pthread_cond_timedwait failed: %s", __FUNCTION__, strerror(ret));
        goto exit;
    }
    if ((ret = pthread_mutex_unlock(&mutex)) != 0) {
        ALOGE("%s: pthread_mutex_unlock failed: %s", __FUNCTION__, strerror(ret));
        goto exit;
    }

    if (status == 0) {
        BTHSVERB("%s: HCI command succeed", __FUNCTION__);
        return BTCELLCOEX_STATUS_OK;
    } else {
        ALOGE("%s: HCI command failed", __FUNCTION__);
        return BTCELLCOEX_STATUS_CMD_FAILED;
    }
exit:
    return BTCELLCOEX_STATUS_UNKNOWN_ERROR;
}


#if (BTHCISERVICE_VERB == TRUE)
/*******************************************************************************
**
** Function         print_xmit
**
** Description      Debug function to print the HCI command length, opcode and parameters
**
** Returns          BT_STATUS_SUCCESS on success
**
*******************************************************************************/
static void print_xmit(HC_BT_HDR *p_msg) {
    uint16_t opcode;
    uint8_t length, i;
    uint8_t *p = (uint8_t *)(p_msg + 1);

    STREAM_TO_UINT16(opcode, p);
    STREAM_TO_UINT8(length, p);
    BTHSVERB("%s: Send a %d bytes long packet. opcode = 0x%04X", __FUNCTION__, length, opcode);
    for(i = 0; i < length; i++) BTHSVERB("0x%02X", *p++);
}
#else
static void print_xmit(HC_BT_HDR *p_msg) {}
#endif
