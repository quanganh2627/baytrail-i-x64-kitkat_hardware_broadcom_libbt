/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
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
 *  Filename:      hardware.c
 *
 *  Description:   Contains controller-specific functions, like
 *                      firmware patch download
 *                      low power mode operations
 *
 ******************************************************************************/

#define LOG_TAG "bt_hwcfg"

#include <utils/Log.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <cutils/properties.h>
#include <stdlib.h>
#include <pthread.h>
#include "bt_hci_bdroid.h"
#include "bt_vendor_brcm.h"
#include "userial.h"
#include "userial_vendor.h"
#include "upio.h"

#include <lct.h>

/******************************************************************************
**  Constants & Macros
******************************************************************************/

#ifndef BTHW_DBG
#define BTHW_DBG FALSE
#endif

#if (BTHW_DBG == TRUE)
#define BTHWDBG(param, ...) {ALOGD(param, ## __VA_ARGS__);}
#else
#define BTHWDBG(param, ...) {}
#endif

#define FW_PATCHFILE_EXTENSION      ".hcd"
#define FW_PATCHFILE_EXTENSION_LEN  4
#define FW_PATCHFILE_PATH_MAXLEN    248 /* Local_Name length of return of
                                           HCI_Read_Local_Name */

#define HCI_CMD_MAX_LEN             258

#define HCI_RESET                               0x0C03
#define HCI_VSC_WRITE_UART_CLOCK_SETTING        0xFC45
#define HCI_VSC_UPDATE_BAUDRATE                 0xFC18
#define HCI_READ_LOCAL_NAME                     0x0C14
#define HCI_VSC_DOWNLOAD_MINIDRV                0xFC2E
#define HCI_VSC_WRITE_BD_ADDR                   0xFC01
#define HCI_VSC_WRITE_SLEEP_MODE                0xFC27
#define HCI_VSC_WRITE_SCO_PCM_INT_PARAM         0xFC1C
#define HCI_VSC_WRITE_PCM_DATA_FORMAT_PARAM     0xFC1E
#define HCI_VSC_WRITE_I2SPCM_INTERFACE_PARAM    0xFC6D
#define HCI_VSC_WRITE_MSBC_ENABLE_PARAM         0xFC7E
#define HCI_VSC_WRITE_RAM                       0xFC4C
#define HCI_VSC_LAUNCH_RAM                      0xFC4E
#define HCI_READ_LOCAL_BDADDR                   0x1009

#define HCI_EVT_CMD_CMPL_STATUS_RET_BYTE        5
#define HCI_EVT_CMD_CMPL_LOCAL_NAME_STRING      6
#define HCI_EVT_CMD_CMPL_LOCAL_REVISION         12
#define HCI_EVT_CMD_CMPL_LOCAL_BDADDR_ARRAY     6
#define HCI_EVT_CMD_CMPL_OPCODE                 3
#define LPM_CMD_PARAM_SIZE                      12
#define UPDATE_BAUDRATE_CMD_PARAM_SIZE          6
#define HCI_CMD_PREAMBLE_SIZE                   3
#define HCD_REC_PAYLOAD_LEN_BYTE                2
#define BD_ADDR_LEN                             6
#define LOCAL_NAME_BUFFER_LEN                   32
#define LOCAL_BDADDR_PATH_BUFFER_LEN            256

#define HCI_READ_LOCAL_VERSION_INFORMATION      0x1001


#define STREAM_TO_UINT16(u16, p) {u16 = ((uint16_t)(*(p)) + (((uint16_t)(*((p) + 1))) << 8)); (p) += 2;}
#define UINT16_TO_STREAM(p, u16) {*(p)++ = (uint8_t)(u16); *(p)++ = (uint8_t)((u16) >> 8);}
#define UINT32_TO_STREAM(p, u32) {*(p)++ = (uint8_t)(u32); *(p)++ = (uint8_t)((u32) >> 8); *(p)++ = (uint8_t)((u32) >> 16); *(p)++ = (uint8_t)((u32) >> 24);}

/******************************************************************************
**  Local type definitions
******************************************************************************/

/* Hardware Configuration State */
enum {
    HW_CFG_START = 1,
    HW_CFG_SET_UART_CLOCK,
    HW_CFG_SET_UART_BAUD_1,
    HW_CFG_READ_LOCAL_NAME,
    HW_CFG_CHECK_LOCAL_REVISION,
    HW_CFG_CHECK_LOCAL_NAME,
    HW_CFG_DL_MINIDRIVER,
    HW_CFG_DL_FW_PATCH,
    HW_CFG_SET_UART_BAUD_2,
    HW_CFG_SET_BD_ADDR
#if (USE_CONTROLLER_BDADDR == TRUE)
    , HW_CFG_READ_BD_ADDR
#endif
};

/* h/w config control block */
typedef struct
{
    uint8_t state;                          /* Hardware configuration state */
    int     fw_fd;                          /* FW patch file fd */
    uint8_t f_set_baud_2;                   /* Baud rate switch state */
    char    local_chip_name[LOCAL_NAME_BUFFER_LEN];
} bt_hw_cfg_cb_t;

/* Hardware SCO Configuration State */
enum hw_sco_state {
    HW_SCO_PCM,
    HW_SCO_PCM_FORMAT,
    HW_SCO_I2S
};

/* Hardware Codec Configuration State */
enum hw_wbs_state {
    HW_WBS_CODEC,
    HW_WBS_PCM,
    HW_WBS_I2S
};

/* low power mode parameters */
typedef struct
{
    uint8_t sleep_mode;                     /* 0(disable),1(UART),9(H5) */
    uint8_t host_stack_idle_threshold;      /* Unit scale 300ms/25ms */
    uint8_t host_controller_idle_threshold; /* Unit scale 300ms/25ms */
    uint8_t bt_wake_polarity;               /* 0=Active Low, 1= Active High */
    uint8_t host_wake_polarity;             /* 0=Active Low, 1= Active High */
    uint8_t allow_host_sleep_during_sco;
    uint8_t combine_sleep_mode_and_lpm;
    uint8_t enable_uart_txd_tri_state;      /* UART_TXD Tri-State */
    uint8_t sleep_guard_time;               /* sleep guard time in 12.5ms */
    uint8_t wakeup_guard_time;              /* wakeup guard time in 12.5ms */
    uint8_t txd_config;                     /* TXD is high in sleep state */
    uint8_t pulsed_host_wake;               /* pulsed host wake if mode = 1 */
} bt_lpm_param_t;

/* Firmware re-launch settlement time */
typedef struct {
    const char *chipset_name;
    const uint32_t delay_time;
} fw_settlement_entry_t;


/******************************************************************************
**  Externs
******************************************************************************/

void hw_config_cback(void *p_evt_buf);
extern uint8_t vnd_local_bd_addr[BD_ADDR_LEN];


/******************************************************************************
**  Static variables
******************************************************************************/

static char fw_patchfile_path[256] = FW_PATCHFILE_LOCATION;
static char fw_patchfile_name[128] = { 0 };
#if (VENDOR_LIB_RUNTIME_TUNING_ENABLED == TRUE)
static int fw_patch_settlement_delay = -1;
#endif

static bt_hw_cfg_cb_t hw_cfg_cb;
static enum hw_sco_state hw_sco_cb_state = 0;
static enum hw_wbs_state hw_wbs_cb_state = 0;
static pthread_mutex_t lpm_mutex = PTHREAD_MUTEX_INITIALIZER;

static bt_lpm_param_t lpm_param =
{
    LPM_SLEEP_MODE,
    LPM_IDLE_THRESHOLD,
    LPM_HC_IDLE_THRESHOLD,
    LPM_BT_WAKE_POLARITY,
    LPM_HOST_WAKE_POLARITY,
    LPM_ALLOW_HOST_SLEEP_DURING_SCO,
    LPM_COMBINE_SLEEP_MODE_AND_LPM,
    LPM_ENABLE_UART_TXD_TRI_STATE,
    0,  /* not applicable */
    0,  /* not applicable */
    0,  /* not applicable */
    LPM_PULSED_HOST_WAKE
};

static uint8_t bt_pcm_sco_param[SCO_PCM_PARAM_SIZE] =
{
    SCO_PCM_ROUTING,
    SCO_PCM_IF_CLOCK_RATE,
    SCO_PCM_IF_FRAME_TYPE,
    SCO_PCM_IF_SYNC_MODE,
    SCO_PCM_IF_CLOCK_MODE
};
/*
 * Parameter names used in bt_vendor.conf to configure BT SCO PCM settings.
 * Below table should always matches the previous table in terms of size
 * and elements order.
 */
static const char* sco_pcm_parameter_name[SCO_PCM_PARAM_SIZE] =
{
    "SCO_PCM_ROUTING",
    "SCO_PCM_IF_CLOCK_RATE",
    "SCO_PCM_IF_FRAME_TYPE",
    "SCO_PCM_IF_SYNC_MODE",
    "SCO_PCM_IF_CLOCK_MODE"
};

static uint8_t bt_pcm_data_fmt_param[PCM_DATA_FORMAT_PARAM_SIZE] =
{
    PCM_DATA_FMT_SHIFT_MODE,
    PCM_DATA_FMT_FILL_BITS,
    PCM_DATA_FMT_FILL_METHOD,
    PCM_DATA_FMT_FILL_NUM,
    PCM_DATA_FMT_JUSTIFY_MODE
};
/*
 * Parameter names used in bt_vendor.conf to configure BT SCO PCM FORMAT settings.
 * Below table should always matches the previous table in terms of size
 * and elements order.
 */
static const char* pcm_data_fmt_parameter_name[PCM_DATA_FORMAT_PARAM_SIZE] =
{
    "PCM_DATA_FMT_SHIFT_MODE",
    "PCM_DATA_FMT_FILL_BITS",
    "PCM_DATA_FMT_FILL_METHOD",
    "PCM_DATA_FMT_FILL_NUM",
    "PCM_DATA_FMT_JUSTIFY_MODE"
};

#if (SCO_USE_I2S_INTERFACE == TRUE)
static uint8_t bt_i2s_sco_param[SCO_I2SPCM_PARAM_SIZE] =
{
    SCO_I2SPCM_IF_MODE,
    SCO_I2SPCM_IF_ROLE,
    SCO_I2SPCM_IF_SAMPLE_RATE,
    SCO_I2SPCM_IF_CLOCK_RATE
};
/*
 * Parameter names use in bt_vendor.conf to configure BT SCO I2S settings.
 * Below table should always match the previous table in term of size
 * and element order.
 */
static const char* sco_i2s_parameter_name[SCO_I2SPCM_PARAM_SIZE] = {
    "SCO_I2SPCM_IF_MODE",
    "SCO_I2SPCM_IF_ROLE",
    "SCO_I2SPCM_IF_SAMPLE_RATE",
    "SCO_I2SPCM_IF_CLOCK_RATE"
};

#endif // (SCO_USE_I2S_INTERFACE == TRUE)

/*
 * Parameters used by the MSBC_ENABLE command to enable mSBC, specified as
 * non-configurable constant values
 */
static uint8_t msbc_enable_param[MSBC_ENABLE_PARAM_SIZE] = {
       1,
       2,
       0
};

/* Parameter used by the MSBC_ENABLE command to disable mSBC */
static uint8_t msbc_disable_param[MSBC_DISABLE_PARAM_SIZE] = {
       0
};

/*
 * The look-up table of recommended firmware settlement delay (milliseconds) on
 * known chipsets.
 */
static const fw_settlement_entry_t fw_settlement_table[] = {
    {"BCM43241", 200},
    {(const char *) NULL, 100}  // Giving the generic fw settlement delay setting.
};

/******************************************************************************
**  Static functions
******************************************************************************/

/******************************************************************************
**  Controller Initialization Static Functions
******************************************************************************/

/*******************************************************************************
**
** Function        look_up_fw_settlement_delay
**
** Description     If FW_PATCH_SETTLEMENT_DELAY_MS has not been explicitly
**                 re-defined in the platform specific build-time configuration
**                 file, we will search into the look-up table for a
**                 recommended firmware settlement delay value.
**
**                 Although the settlement time might be also related to board
**                 configurations such as the crystal clocking speed.
**
** Returns         Firmware settlement delay
**
*******************************************************************************/
uint32_t look_up_fw_settlement_delay (void)
{
    uint32_t ret_value;
    fw_settlement_entry_t *p_entry;

    if (FW_PATCH_SETTLEMENT_DELAY_MS > 0)
    {
        ret_value = FW_PATCH_SETTLEMENT_DELAY_MS;
    }
#if (VENDOR_LIB_RUNTIME_TUNING_ENABLED == TRUE)
    else if (fw_patch_settlement_delay >= 0)
    {
        ret_value = fw_patch_settlement_delay;
    }
#endif
    else
    {
        p_entry = (fw_settlement_entry_t *)fw_settlement_table;

        while (p_entry->chipset_name != NULL)
        {
            if (strstr(hw_cfg_cb.local_chip_name, p_entry->chipset_name)!=NULL)
            {
                break;
            }

            p_entry++;
        }

        ret_value = p_entry->delay_time;
    }

    BTHWDBG( "Settlement delay -- %d ms", ret_value);

    return (ret_value);
}

/*******************************************************************************
**
** Function        ms_delay
**
** Description     sleep unconditionally for timeout milliseconds
**
** Returns         None
**
*******************************************************************************/
void ms_delay (uint32_t timeout)
{
    struct timespec delay;
    int err;

    if (timeout == 0)
        return;

    delay.tv_sec = timeout / 1000;
    delay.tv_nsec = 1000 * 1000 * (timeout%1000);

    /* [u]sleep can't be used because it uses SIGALRM */
    do {
        err = nanosleep(&delay, &delay);
    } while (err < 0 && errno ==EINTR);
}

/*******************************************************************************
**
** Function        line_speed_to_userial_baud
**
** Description     helper function converts line speed number into USERIAL baud
**                 rate symbol
**
** Returns         unit8_t (USERIAL baud symbol)
**
*******************************************************************************/
uint8_t line_speed_to_userial_baud(uint32_t line_speed)
{
    uint8_t baud;

    if (line_speed == 4000000)
        baud = USERIAL_BAUD_4M;
    else if (line_speed == 3000000)
        baud = USERIAL_BAUD_3M;
    else if (line_speed == 2000000)
        baud = USERIAL_BAUD_2M;
    else if (line_speed == 1000000)
        baud = USERIAL_BAUD_1M;
    else if (line_speed == 921600)
        baud = USERIAL_BAUD_921600;
    else if (line_speed == 460800)
        baud = USERIAL_BAUD_460800;
    else if (line_speed == 230400)
        baud = USERIAL_BAUD_230400;
    else if (line_speed == 115200)
        baud = USERIAL_BAUD_115200;
    else if (line_speed == 57600)
        baud = USERIAL_BAUD_57600;
    else if (line_speed == 19200)
        baud = USERIAL_BAUD_19200;
    else if (line_speed == 9600)
        baud = USERIAL_BAUD_9600;
    else if (line_speed == 1200)
        baud = USERIAL_BAUD_1200;
    else if (line_speed == 600)
        baud = USERIAL_BAUD_600;
    else
    {
        ALOGE( "userial vendor: unsupported baud speed %d", line_speed);
        baud = USERIAL_BAUD_115200;
    }

    return baud;
}


/*******************************************************************************
**
** Function         hw_strncmp
**
** Description      Used to compare two strings in caseless
**
** Returns          0: match, otherwise: not match
**
*******************************************************************************/
static int hw_strncmp (const char *p_str1, const char *p_str2, const int len)
{
    int i;

    if (!p_str1 || !p_str2)
        return (1);

    for (i = 0; i < len; i++)
    {
        if (toupper(p_str1[i]) != toupper(p_str2[i]))
            return (i+1);
    }

    return 0;
}

/*******************************************************************************
**
** Function         hw_config_findpatch
**
** Description      Search for a proper firmware patch file
**                  The selected firmware patch file name with full path
**                  will be stored in the input string parameter, i.e.
**                  p_chip_id_str, when returns.
**
** Returns          TRUE when found the target patch file, otherwise FALSE
**
*******************************************************************************/
static uint8_t hw_config_findpatch(char *p_chip_id_str)
{
    DIR *dirp;
    struct dirent *dp;
    int filenamelen;
    uint8_t retval = FALSE;

    BTHWDBG("Target name = [%s]", p_chip_id_str);

    if (strlen(fw_patchfile_name)> 0)
    {
        /* If specific filepath and filename have been given in run-time
         * configuration /etc/bluetooth/bt_vendor.conf file, we will use them
         * to concatenate the filename to open rather than searching a file
         * matching to chipset name in the fw_patchfile_path folder.
         */
        sprintf(p_chip_id_str, "%s", fw_patchfile_path);
        if (fw_patchfile_path[strlen(fw_patchfile_path)- 1] != '/')
        {
            strcat(p_chip_id_str, "/");
        }
        strcat(p_chip_id_str, fw_patchfile_name);

        ALOGI("FW patchfile: %s", p_chip_id_str);
        return TRUE;
    }

    if ((dirp = opendir(fw_patchfile_path)) != NULL)
    {
        /* Fetch next filename in patchfile directory */
        while ((dp = readdir(dirp)) != NULL)
        {
            /* Check if filename starts with chip-id name */
            if ((hw_strncmp(dp->d_name, p_chip_id_str, strlen(p_chip_id_str)) \
                ) == 0)
            {
                /* Check if it has .hcd extenstion */
                filenamelen = strlen(dp->d_name);
                if ((filenamelen >= FW_PATCHFILE_EXTENSION_LEN) &&
                    ((hw_strncmp(
                          &dp->d_name[filenamelen-FW_PATCHFILE_EXTENSION_LEN], \
                          FW_PATCHFILE_EXTENSION, \
                          FW_PATCHFILE_EXTENSION_LEN) \
                     ) == 0))
                {
                    ALOGI("Found patchfile: %s/%s", \
                        fw_patchfile_path, dp->d_name);

                    /* Make sure length does not exceed maximum */
                    if ((filenamelen + strlen(fw_patchfile_path)) > \
                         FW_PATCHFILE_PATH_MAXLEN)
                    {
                        ALOGE("Invalid patchfile name (too long)");
                    }
                    else
                    {
                        memset(p_chip_id_str, 0, FW_PATCHFILE_PATH_MAXLEN);
                        /* Found patchfile. Store location and name */
                        strcpy(p_chip_id_str, fw_patchfile_path);
                        if (fw_patchfile_path[ \
                            strlen(fw_patchfile_path)- 1 \
                            ] != '/')
                        {
                            strcat(p_chip_id_str, "/");
                        }
                        strcat(p_chip_id_str, dp->d_name);
                        retval = TRUE;
                    }
                    break;
                }
            }
        }

        closedir(dirp);

        if (retval == FALSE)
        {
            /* Try again chip name without revision info */

            int len = strlen(p_chip_id_str);
            char *p = p_chip_id_str + len - 1;

            /* Scan backward and look for the first alphabet
               which is not M or m
            */
            while (len > 3) // BCM****
            {
                if ((isdigit(*p)==0) && (*p != 'M') && (*p != 'm'))
                    break;

                p--;
                len--;
            }

            if (len > 3)
            {
                *p = 0;
                retval = hw_config_findpatch(p_chip_id_str);
            }
        }
    }
    else
    {
        ALOGE("Could not open %s", fw_patchfile_path);
    }

    return (retval);
}

/*******************************************************************************
**
** Function         hw_config_set_bdaddr
**
** Description      Program controller's Bluetooth Device Address
**
** Returns          TRUE, if valid address is sent
**                  FALSE, otherwise
**
*******************************************************************************/
static uint8_t hw_config_set_bdaddr(HC_BT_HDR *p_buf)
{
    uint8_t retval = FALSE;
    uint8_t *p = (uint8_t *) (p_buf + 1);

    ALOGI("Setting local bd addr to %02X:%02X:%02X:%02X:%02X:%02X",
        vnd_local_bd_addr[0], vnd_local_bd_addr[1], vnd_local_bd_addr[2],
        vnd_local_bd_addr[3], vnd_local_bd_addr[4], vnd_local_bd_addr[5]);

    UINT16_TO_STREAM(p, HCI_VSC_WRITE_BD_ADDR);
    *p++ = BD_ADDR_LEN; /* parameter length */
    *p++ = vnd_local_bd_addr[5];
    *p++ = vnd_local_bd_addr[4];
    *p++ = vnd_local_bd_addr[3];
    *p++ = vnd_local_bd_addr[2];
    *p++ = vnd_local_bd_addr[1];
    *p = vnd_local_bd_addr[0];

    p_buf->len = HCI_CMD_PREAMBLE_SIZE + BD_ADDR_LEN;
    hw_cfg_cb.state = HW_CFG_SET_BD_ADDR;

    retval = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_BD_ADDR, p_buf, \
                                 hw_config_cback);

    return (retval);
}

/*******************************************************************************
**
** Function         hw_config_set_baudrate
**
** Description      Change controller's UART baud rate
**
** Returns          TRUE, if command is sent
**                  FALSE, otherwise
**
*******************************************************************************/
static uint8_t hw_config_set_baudrate(HC_BT_HDR *p_buf)
{
    uint8_t retval = FALSE;
    uint8_t *p = (uint8_t *) (p_buf + 1);

    if (hw_cfg_cb.state != HW_CFG_SET_UART_CLOCK)
    {
        /* Check if we need to set UART clock first */
        if (UART_TARGET_BAUD_RATE > 3000000)
        {
            /* set UART clock to 48MHz */
            UINT16_TO_STREAM(p, HCI_VSC_WRITE_UART_CLOCK_SETTING);
            *p++ = 1; /* parameter length */
            *p = 1; /* (1,"UART CLOCK 48 MHz")(2,"UART CLOCK 24 MHz") */

            p_buf->len = HCI_CMD_PREAMBLE_SIZE + 1;
            hw_cfg_cb.state = HW_CFG_SET_UART_CLOCK;

            retval = bt_vendor_cbacks->xmit_cb( \
                                HCI_VSC_WRITE_UART_CLOCK_SETTING, \
                                p_buf, hw_config_cback);
            return (retval);
        }
    }

    /* set controller's UART baud rate to 3M */
    UINT16_TO_STREAM(p, HCI_VSC_UPDATE_BAUDRATE);
    *p++ = UPDATE_BAUDRATE_CMD_PARAM_SIZE; /* parameter length */
    *p++ = 0; /* encoded baud rate */
    *p++ = 0; /* use encoded form */
    UINT32_TO_STREAM(p, UART_TARGET_BAUD_RATE);

    p_buf->len = HCI_CMD_PREAMBLE_SIZE + UPDATE_BAUDRATE_CMD_PARAM_SIZE;
    hw_cfg_cb.state = (hw_cfg_cb.f_set_baud_2) ? \
                HW_CFG_SET_UART_BAUD_2 : HW_CFG_SET_UART_BAUD_1;

    retval = bt_vendor_cbacks->xmit_cb(HCI_VSC_UPDATE_BAUDRATE, \
                                        p_buf, hw_config_cback);

    return (retval);
}

#if (USE_CONTROLLER_BDADDR == TRUE)
/*******************************************************************************
**
** Function         hw_config_read_bdaddr
**
** Description      Read controller's Bluetooth Device Address
**
** Returns          TRUE, if valid address is sent
**                  FALSE, otherwise
**
*******************************************************************************/
static uint8_t hw_config_read_bdaddr(HC_BT_HDR *p_buf)
{
    uint8_t retval = FALSE;
    uint8_t *p = (uint8_t *) (p_buf + 1);

    UINT16_TO_STREAM(p, HCI_READ_LOCAL_BDADDR);
    *p = 0; /* parameter length */

    p_buf->len = HCI_CMD_PREAMBLE_SIZE;
    hw_cfg_cb.state = HW_CFG_READ_BD_ADDR;

    retval = bt_vendor_cbacks->xmit_cb(HCI_READ_LOCAL_BDADDR, p_buf, \
                                 hw_config_cback);

    return (retval);
}
#endif // (USE_CONTROLLER_BDADDR == TRUE)

/*******************************************************************************
**
** Function         hw_config_cback
**
** Description      Callback function for controller configuration
**
** Returns          None
**
*******************************************************************************/
void hw_config_cback(void *p_mem)
{
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
    char        *p_name, *p_tmp;
    uint8_t     *p, status;
    uint16_t    opcode;
    HC_BT_HDR  *p_buf=NULL;
    uint8_t     is_proceeding = FALSE;
    int         i;
#if (USE_CONTROLLER_BDADDR == TRUE)
    const uint8_t null_bdaddr[BD_ADDR_LEN] = {0,0,0,0,0,0};
#endif

    status = *((uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_STATUS_RET_BYTE);
    p = (uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_OPCODE;
    STREAM_TO_UINT16(opcode,p);

    /* Ask a new buffer big enough to hold any HCI commands sent in here */
    if ((status == 0) && bt_vendor_cbacks)
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                       HCI_CMD_MAX_LEN);

    if (p_buf != NULL)
    {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->len = 0;
        p_buf->layer_specific = 0;

        p = (uint8_t *) (p_buf + 1);

        switch (hw_cfg_cb.state)
        {
            case HW_CFG_START:
                /* read local name */
                UINT16_TO_STREAM(p, HCI_READ_LOCAL_NAME);
                *p = 0; /* parameter length */

                p_buf->len = HCI_CMD_PREAMBLE_SIZE;
                hw_cfg_cb.state = HW_CFG_READ_LOCAL_NAME;

                is_proceeding = bt_vendor_cbacks->xmit_cb(HCI_READ_LOCAL_NAME, \
                                                    p_buf, hw_config_cback);
                break;

            case HW_CFG_READ_LOCAL_NAME:
                p_tmp = p_name = (char *) (p_evt_buf + 1) + \
                         HCI_EVT_CMD_CMPL_LOCAL_NAME_STRING;

                for (i=0; (i < LOCAL_NAME_BUFFER_LEN)||(*(p_name+i) != 0); i++)
                    *(p_name+i) = toupper(*(p_name+i));

                if ((p_name = strstr(p_name, "BCM")) != NULL)
                {
                    strncpy(hw_cfg_cb.local_chip_name, p_name, \
                            LOCAL_NAME_BUFFER_LEN-1);
                }
                else
                {
                    strncpy(hw_cfg_cb.local_chip_name, "UNKNOWN", \
                            LOCAL_NAME_BUFFER_LEN-1);
                    break;
                }

                hw_cfg_cb.local_chip_name[LOCAL_NAME_BUFFER_LEN-1] = 0;

                /* Additional check for revision if chip is BCM4335 */
                if (strstr(hw_cfg_cb.local_chip_name, "BCM4335") != NULL)
                {
                    ALOGI("bt vendor lib: BCM4335 chip detected, needs to check for the lmp version...");

                    /* read local revision to check lmp version to differentiate between A0 and B0 revision of BCM4335 */
                    UINT16_TO_STREAM(p, HCI_READ_LOCAL_VERSION_INFORMATION);
                    *p = 0; /* parameter length */

                    p_buf->len = HCI_CMD_PREAMBLE_SIZE;
                    hw_cfg_cb.state = HW_CFG_CHECK_LOCAL_REVISION;

                    is_proceeding = bt_vendor_cbacks->xmit_cb( \
                                        HCI_READ_LOCAL_VERSION_INFORMATION, \
                                        p_buf, hw_config_cback);
                    break;
                }
                goto check_local_name;


            case HW_CFG_CHECK_LOCAL_REVISION:
                {
                    uint16_t    lmp_subversion;
                    uint8_t     *p_lmp;
                    char tmp[5];

                    p_lmp = (uint8_t *) (p_evt_buf + 1) + HCI_EVT_CMD_CMPL_LOCAL_REVISION;
                    STREAM_TO_UINT16(lmp_subversion, p_lmp);
                    ALOGI("bt vendor lib: lmp version : %04x.", lmp_subversion);
                    if (lmp_subversion == 0x4106)
                    {
                        /* Found BCM4335B0 revision */
                        hw_cfg_cb.local_chip_name[7] = 'B';
                    }

                    snprintf(tmp, sizeof(tmp), "%04x", lmp_subversion);
                    lct_log(CT_EV_INFO, "cws.bt", "fw_version", 0, hw_cfg_cb.local_chip_name, tmp);
                }
                /* fall through intentionally */

            case HW_CFG_CHECK_LOCAL_NAME:

check_local_name:
                {
                    char tmp_path[255];

                    BTHWDBG("Chipset %s", hw_cfg_cb.local_chip_name);

                    strcpy(tmp_path, hw_cfg_cb.local_chip_name);

                    if ((status = hw_config_findpatch(tmp_path)) == TRUE)
                    {
                        if ((hw_cfg_cb.fw_fd = open(tmp_path, O_RDONLY)) == -1)
                        {
                            ALOGE("vendor lib preload failed to open [%s]", tmp_path);
                            lct_log(CT_EV_STAT, "cws.bt", "fw_error", 0, tmp_path);
                        }
                    }
                    else
                    {
                        ALOGE( \
                        "vendor lib preload failed to locate firmware patch file" \
                        );
                        lct_log(CT_EV_STAT, "cws.bt", "fw_error", 0, tmp_path);
                    }
                }

                if (is_proceeding == FALSE)
                {
                    is_proceeding = hw_config_set_baudrate(p_buf);
                }
                break;

            case HW_CFG_SET_UART_BAUD_1:
                /* update baud rate of host's UART port */
                ALOGI("bt vendor lib: set UART baud %i", UART_TARGET_BAUD_RATE);
                userial_vendor_set_baud( \
                    line_speed_to_userial_baud(UART_TARGET_BAUD_RATE) \
                );

                if (hw_cfg_cb.fw_fd != -1)
                {
                    /* vsc_download_minidriver */
                    UINT16_TO_STREAM(p, HCI_VSC_DOWNLOAD_MINIDRV);
                    *p = 0; /* parameter length */

                    p_buf->len = HCI_CMD_PREAMBLE_SIZE;
                    hw_cfg_cb.state = HW_CFG_DL_MINIDRIVER;

                    is_proceeding = bt_vendor_cbacks->xmit_cb( \
                                        HCI_VSC_DOWNLOAD_MINIDRV, p_buf, \
                                        hw_config_cback);
                }
                else
                {
                    is_proceeding = hw_config_set_bdaddr(p_buf);
                }
                break;

            case HW_CFG_DL_MINIDRIVER:
                /* give time for placing firmware in download mode */
                ms_delay(50);
                hw_cfg_cb.state = HW_CFG_DL_FW_PATCH;
                /* fall through intentionally */
            case HW_CFG_DL_FW_PATCH:
                p_buf->len = read(hw_cfg_cb.fw_fd, p, HCI_CMD_PREAMBLE_SIZE);
                if (p_buf->len > 0)
                {
                    if ((p_buf->len < HCI_CMD_PREAMBLE_SIZE) || \
                        (opcode == HCI_VSC_LAUNCH_RAM))
                    {
                        ALOGW("firmware patch file might be altered!");
                    }
                    else
                    {
                        p_buf->len += read(hw_cfg_cb.fw_fd, \
                                           p+HCI_CMD_PREAMBLE_SIZE,\
                                           *(p+HCD_REC_PAYLOAD_LEN_BYTE));
                        STREAM_TO_UINT16(opcode,p);
                        is_proceeding = bt_vendor_cbacks->xmit_cb(opcode, \
                                                p_buf, hw_config_cback);
                        break;
                    }
                }

                close(hw_cfg_cb.fw_fd);
                hw_cfg_cb.fw_fd = -1;

                /* Normally the firmware patch configuration file
                 * sets the new starting baud rate at 115200.
                 * So, we need update host's baud rate accordingly.
                 */
                ALOGI("bt vendor lib: set UART baud 115200");
                userial_vendor_set_baud(USERIAL_BAUD_115200);

                /* Next, we would like to boost baud rate up again
                 * to desired working speed.
                 */
                hw_cfg_cb.f_set_baud_2 = TRUE;

                /* Check if we need to pause a few hundred milliseconds
                 * before sending down any HCI command.
                 */
                ms_delay(look_up_fw_settlement_delay());

                /* fall through intentionally */
            case HW_CFG_SET_UART_CLOCK:
                is_proceeding = hw_config_set_baudrate(p_buf);
                break;

            case HW_CFG_SET_UART_BAUD_2:
                /* update baud rate of host's UART port */
                ALOGI("bt vendor lib: set UART baud %i", UART_TARGET_BAUD_RATE);
                userial_vendor_set_baud( \
                    line_speed_to_userial_baud(UART_TARGET_BAUD_RATE) \
                );

#if (USE_CONTROLLER_BDADDR == TRUE)
                if ((is_proceeding = hw_config_read_bdaddr(p_buf)) == TRUE)
                    break;
#else
                if ((is_proceeding = hw_config_set_bdaddr(p_buf)) == TRUE)
                    break;
#endif
                /* fall through intentionally */
            case HW_CFG_SET_BD_ADDR:
                ALOGI("vendor lib fwcfg completed");
                bt_vendor_cbacks->dealloc(p_buf);
                bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);

                hw_cfg_cb.state = 0;

                if (hw_cfg_cb.fw_fd != -1)
                {
                    close(hw_cfg_cb.fw_fd);
                    hw_cfg_cb.fw_fd = -1;
                }

                is_proceeding = TRUE;
                break;

#if (USE_CONTROLLER_BDADDR == TRUE)
            case HW_CFG_READ_BD_ADDR:
                p_tmp = (char *) (p_evt_buf + 1) + \
                         HCI_EVT_CMD_CMPL_LOCAL_BDADDR_ARRAY;

                if (memcmp(p_tmp, null_bdaddr, BD_ADDR_LEN) == 0)
                {
                    // Controller does not have a valid OTP BDADDR!
                    // Set the BTIF initial BDADDR instead.
                    if ((is_proceeding = hw_config_set_bdaddr(p_buf)) == TRUE)
                        break;
                }
                else
                {
                    ALOGI("Controller OTP bdaddr %02X:%02X:%02X:%02X:%02X:%02X",
                        *(p_tmp+5), *(p_tmp+4), *(p_tmp+3),
                        *(p_tmp+2), *(p_tmp+1), *p_tmp);
                }

                ALOGI("vendor lib fwcfg completed");
                bt_vendor_cbacks->dealloc(p_buf);
                bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);

                hw_cfg_cb.state = 0;

                if (hw_cfg_cb.fw_fd != -1)
                {
                    close(hw_cfg_cb.fw_fd);
                    hw_cfg_cb.fw_fd = -1;
                }

                is_proceeding = TRUE;
                break;
#endif // (USE_CONTROLLER_BDADDR == TRUE)
        } // switch(hw_cfg_cb.state)
    } // if (p_buf != NULL)

    /* Free the RX event buffer */
    if (bt_vendor_cbacks)
        bt_vendor_cbacks->dealloc(p_evt_buf);

    if (is_proceeding == FALSE)
    {
        ALOGE("vendor lib fwcfg aborted!!!");
        lct_log(CT_EV_STAT, "cws.bt", "fw_cfg", 0);
        if (bt_vendor_cbacks)
        {
            if (p_buf != NULL)
                bt_vendor_cbacks->dealloc(p_buf);

            bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_FAIL);
        }

        if (hw_cfg_cb.fw_fd != -1)
        {
            close(hw_cfg_cb.fw_fd);
            hw_cfg_cb.fw_fd = -1;
        }

        hw_cfg_cb.state = 0;
    }
}

/******************************************************************************
**   LPM Static Functions
******************************************************************************/

/*******************************************************************************
**
** Function         hw_lpm_ctrl_cback
**
** Description      Callback function for lpm enable/disable rquest
**
** Returns          None
**
*******************************************************************************/
void hw_lpm_ctrl_cback(void *p_mem)
{
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
    bt_vendor_op_result_t status = BT_VND_OP_RESULT_FAIL;

    if (*((uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_STATUS_RET_BYTE) == 0)
    {
        status = BT_VND_OP_RESULT_SUCCESS;
    }
    pthread_mutex_unlock(&lpm_mutex);

    if (bt_vendor_cbacks)
    {
        bt_vendor_cbacks->lpm_cb(status);
        bt_vendor_cbacks->dealloc(p_evt_buf);
    }
}


#if (SCO_CFG_INCLUDED == TRUE)
/*****************************************************************************
**   SCO Configuration Static Functions
*****************************************************************************/

/*******************************************************************************
**
** Function         hw_sco_cfg_cback
**
** Description      Callback function for SCO configuration rquest
**
** Returns          None
**
*******************************************************************************/
void hw_sco_cfg_cback(void *p_mem)
{
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
    uint8_t     *p;
    uint16_t    opcode;
    HC_BT_HDR  *p_buf=NULL;
    uint8_t     ret;

    p = (uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_OPCODE;
    STREAM_TO_UINT16(opcode,p);
    if (bt_vendor_cbacks)
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                       HCI_CMD_MAX_LEN);
    if (p_buf != NULL) {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->layer_specific = 0;
        p_buf->len = HCI_CMD_PREAMBLE_SIZE;

        switch (hw_sco_cb_state) {

            case HW_SCO_PCM: {
                BTHWDBG("HW_SCO_PCM");
                if (opcode == HCI_VSC_WRITE_SCO_PCM_INT_PARAM) {
                    p_buf->len += PCM_DATA_FORMAT_PARAM_SIZE;
                    p = (uint8_t *) (p_buf + 1);
                    UINT16_TO_STREAM(p, HCI_VSC_WRITE_PCM_DATA_FORMAT_PARAM);
                    *p++ = PCM_DATA_FORMAT_PARAM_SIZE;
                    memcpy(p, &bt_pcm_data_fmt_param, PCM_DATA_FORMAT_PARAM_SIZE);
                    hw_sco_cb_state = HW_SCO_PCM_FORMAT;
                    if ((ret = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_PCM_DATA_FORMAT_PARAM,\
                                           p_buf, hw_sco_cfg_cback)) == FALSE)
                   {
                       bt_vendor_cbacks->dealloc(p_buf);
                   }
                }
            }
            break;

            case HW_SCO_PCM_FORMAT: {
                BTHWDBG("HW_SCO_PCO_FORMAT");
                if (opcode == HCI_VSC_WRITE_PCM_DATA_FORMAT_PARAM) {
#if (SCO_USE_I2S_INTERFACE == TRUE)
                    p_buf->len += SCO_I2SPCM_PARAM_SIZE;
                    p = (uint8_t *) (p_buf + 1);
                    UINT16_TO_STREAM(p, HCI_VSC_WRITE_I2SPCM_INTERFACE_PARAM);
                    *p++ = SCO_I2SPCM_PARAM_SIZE;
                    memcpy(p, &bt_i2s_sco_param, SCO_I2SPCM_PARAM_SIZE);
                    ALOGI("SCO over I2SPCM interface {%d, %d, %d, %d}",
                        bt_i2s_sco_param[0], bt_i2s_sco_param[1], bt_i2s_sco_param[2], bt_i2s_sco_param[3]);
                    hw_sco_cb_state = HW_SCO_I2S;
                    if ((ret = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_I2SPCM_INTERFACE_PARAM,\
                                           p_buf, hw_sco_cfg_cback)) == FALSE)
                   {
                       bt_vendor_cbacks->dealloc(p_buf);
                   }
                }
            }
            break;

            case HW_SCO_I2S: {
                BTHWDBG("HW_SCO_I2S");
                bt_vendor_cbacks->dealloc(p_buf);
                if (opcode == HCI_VSC_WRITE_I2SPCM_INTERFACE_PARAM) {
#endif // (SCO_USE_I2S_INTERFACE == TRUE)
                    bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
                }
            }
            break;
        }
    }
    /* Free the RX event buffer */
    if (bt_vendor_cbacks)
        bt_vendor_cbacks->dealloc(p_evt_buf);
}
#endif // SCO_CFG_INCLUDED

/*****************************************************************************
**   Hardware Configuration Interface Functions
*****************************************************************************/


/*******************************************************************************
**
** Function        hw_config_start
**
** Description     Kick off controller initialization process
**
** Returns         None
**
*******************************************************************************/
void hw_config_start(void)
{
    HC_BT_HDR  *p_buf = NULL;
    uint8_t     *p;

    hw_cfg_cb.state = 0;
    hw_cfg_cb.fw_fd = -1;
    hw_cfg_cb.f_set_baud_2 = FALSE;

    /* Start from sending HCI_RESET */

    if (bt_vendor_cbacks)
    {
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                       HCI_CMD_PREAMBLE_SIZE);
    }

    if (p_buf)
    {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->layer_specific = 0;
        p_buf->len = HCI_CMD_PREAMBLE_SIZE;

        p = (uint8_t *) (p_buf + 1);
        UINT16_TO_STREAM(p, HCI_RESET);
        *p = 0; /* parameter length */

        hw_cfg_cb.state = HW_CFG_START;

        bt_vendor_cbacks->xmit_cb(HCI_RESET, p_buf, hw_config_cback);
    }
    else
    {
        if (bt_vendor_cbacks)
        {
            ALOGE("vendor lib fw conf aborted [no buffer]");
            bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_FAIL);
        }
    }
}

/*******************************************************************************
**
** Function        hw_config_cleanup
**
** Description     Clean up system resource allocated in HW CONFIG module
**
** Returns         None
**
*******************************************************************************/
void hw_config_cleanup(void)
{
    if ((hw_cfg_cb.fw_fd != -1) && (hw_cfg_cb.fw_fd > 2 /*stderr*/))
    {
        close(hw_cfg_cb.fw_fd);
        hw_cfg_cb.fw_fd = -1;
    }
}

/*******************************************************************************
**
** Function        hw_lpm_enable
**
** Description     Enalbe/Disable LPM
**
** Returns         TRUE/FALSE
**
*******************************************************************************/
uint8_t hw_lpm_enable(uint8_t turn_on)
{
    HC_BT_HDR  *p_buf = NULL;
    uint8_t     *p;
    uint8_t     ret = FALSE;

    if (bt_vendor_cbacks)
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                       HCI_CMD_PREAMBLE_SIZE + \
                                                       LPM_CMD_PARAM_SIZE);

    if (p_buf)
    {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->layer_specific = 0;
        p_buf->len = HCI_CMD_PREAMBLE_SIZE + LPM_CMD_PARAM_SIZE;

        p = (uint8_t *) (p_buf + 1);
        UINT16_TO_STREAM(p, HCI_VSC_WRITE_SLEEP_MODE);
        *p++ = LPM_CMD_PARAM_SIZE; /* parameter length */

        if (turn_on)
        {
            memcpy(p, &lpm_param, LPM_CMD_PARAM_SIZE);
            upio_set(UPIO_LPM_MODE, UPIO_ASSERT, 0);
        }
        else
        {
            memset(p, 0, LPM_CMD_PARAM_SIZE);
            upio_set(UPIO_LPM_MODE, UPIO_DEASSERT, 0);
        }
        if (!pthread_mutex_lock(&lpm_mutex)) {
            if ((ret = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_SLEEP_MODE, p_buf, \
                                        hw_lpm_ctrl_cback)) == FALSE)
            {
                bt_vendor_cbacks->dealloc(p_buf);
                pthread_mutex_unlock(&lpm_mutex);
            }
        }
        if (!turn_on)
        {
            pthread_mutex_unlock(&lpm_mutex); //sleep doesn't have a callback to unlock
        }
    }

    if ((ret == FALSE) && bt_vendor_cbacks)
        bt_vendor_cbacks->lpm_cb(BT_VND_OP_RESULT_FAIL);

    return ret;
}

/*******************************************************************************
**
** Function        hw_lpm_get_idle_timeout
**
** Description     Calculate idle time based on host stack idle threshold
**
** Returns         idle timeout value
**
*******************************************************************************/
uint32_t hw_lpm_get_idle_timeout(void)
{
    uint32_t timeout_ms;

    /* set idle time to be LPM_IDLE_TIMEOUT_MULTIPLE times of
     * host stack idle threshold (in 300ms/25ms)
     */
    timeout_ms = (uint32_t)lpm_param.host_stack_idle_threshold \
                            * LPM_IDLE_TIMEOUT_MULTIPLE;

    if (strstr(hw_cfg_cb.local_chip_name, "BCM4325") != NULL)
        timeout_ms *= 25; // 12.5 or 25 ?
    else
        timeout_ms *= 300;

    return timeout_ms;
}

/*******************************************************************************
**
** Function        hw_lpm_set_wake_state
**
** Description     Assert/Deassert BT_WAKE
**
** Returns         None
**
*******************************************************************************/
void hw_lpm_set_wake_state(uint8_t wake_assert)
{
    uint8_t state = (wake_assert) ? UPIO_ASSERT : UPIO_DEASSERT;

    upio_set(UPIO_BT_WAKE, state, lpm_param.bt_wake_polarity);
}

#if (SCO_CFG_INCLUDED == TRUE)
/*******************************************************************************
**
** Function         hw_sco_config
**
** Description      Configure SCO related hardware settings
**
** Returns          None
**
*******************************************************************************/
void hw_sco_config(void)
{
    HC_BT_HDR  *p_buf = NULL;
    uint8_t     *p, ret;

    uint16_t cmd_u16 = HCI_CMD_PREAMBLE_SIZE + SCO_PCM_PARAM_SIZE;

    if (bt_vendor_cbacks)
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE+cmd_u16);

    if (p_buf)
    {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->layer_specific = 0;
        p_buf->len = cmd_u16;

        p = (uint8_t *) (p_buf + 1);
        UINT16_TO_STREAM(p, HCI_VSC_WRITE_SCO_PCM_INT_PARAM);
        *p++ = SCO_PCM_PARAM_SIZE;
        memcpy(p, &bt_pcm_sco_param, SCO_PCM_PARAM_SIZE);
        cmd_u16 = HCI_VSC_WRITE_SCO_PCM_INT_PARAM;
        ALOGI("SCO PCM configure {%d, %d, %d, %d, %d}",
           bt_pcm_sco_param[0], bt_pcm_sco_param[1], bt_pcm_sco_param[2], bt_pcm_sco_param[3], \
           bt_pcm_sco_param[4]);

        hw_sco_cb_state = HW_SCO_PCM;

        if ((ret=bt_vendor_cbacks->xmit_cb(cmd_u16, p_buf, hw_sco_cfg_cback)) \
             == FALSE)
        {
            bt_vendor_cbacks->dealloc(p_buf);
            pthread_mutex_unlock(&lpm_mutex);
        }
        else
            return;
    }

    if (bt_vendor_cbacks)
    {
        ALOGE("vendor lib scocfg aborted");
        bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_FAIL);
    }
}
#endif  // SCO_CFG_INCLUDED

#if (SCO_USE_I2S_INTERFACE == TRUE)
/*******************************************************************************
**
** Function         hw_enable_mSBC_codec_cback
**
** Description      Callback function for enabling/disabling mSBC codec
**
** Returns          None
**
*******************************************************************************/
void hw_enable_mSBC_codec_cback(void *p_mem)
{
    HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
    uint8_t     *p;
    uint16_t    opcode;
    HC_BT_HDR  *p_buf=NULL;
    uint8_t     ret;

    p = (uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_OPCODE;
    STREAM_TO_UINT16(opcode,p);
    if (bt_vendor_cbacks)
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                       HCI_CMD_MAX_LEN);
    if (p_buf != NULL) {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->layer_specific = 0;
        p_buf->len = HCI_CMD_PREAMBLE_SIZE;

        switch (hw_wbs_cb_state) {

            case HW_WBS_CODEC: {
                BTHWDBG("HW_WBS_CODEC");
                if (opcode == HCI_VSC_WRITE_MSBC_ENABLE_PARAM) {
                    p_buf->len += SCO_PCM_PARAM_SIZE;
                    p = (uint8_t *) (p_buf + 1);
                    UINT16_TO_STREAM(p, HCI_VSC_WRITE_SCO_PCM_INT_PARAM);
                    *p++ = SCO_PCM_PARAM_SIZE;
                    memcpy(p, &bt_pcm_sco_param, SCO_PCM_PARAM_SIZE);
                    ALOGI("SCO over PCM interface {%d, %d, %d, %d, %d}",
                        bt_pcm_sco_param[0], bt_pcm_sco_param[1], bt_pcm_sco_param[2], \
                        bt_pcm_sco_param[3], bt_pcm_sco_param[4]);
                    hw_wbs_cb_state = HW_WBS_PCM;
                    if ((ret = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_SCO_PCM_INT_PARAM,\
                                           p_buf, hw_enable_mSBC_codec_cback)) == FALSE)
                   {
                       bt_vendor_cbacks->dealloc(p_buf);
                   }
               }
            }
            break;

            case HW_WBS_PCM: {
                BTHWDBG("HW_WBS_PCM");
                if (opcode == HCI_VSC_WRITE_SCO_PCM_INT_PARAM) {
                    p_buf->len += SCO_I2SPCM_PARAM_SIZE;
                    p = (uint8_t *) (p_buf + 1);
                    UINT16_TO_STREAM(p, HCI_VSC_WRITE_I2SPCM_INTERFACE_PARAM);
                    *p++ = SCO_I2SPCM_PARAM_SIZE;
                    memcpy(p, &bt_i2s_sco_param, SCO_I2SPCM_PARAM_SIZE);
                    ALOGI("SCO over I2SPCM interface {%d, %d, %d, %d}",
                        bt_i2s_sco_param[0], bt_i2s_sco_param[1], bt_i2s_sco_param[2], bt_i2s_sco_param[3]);
                    hw_wbs_cb_state = HW_WBS_I2S;
                    if ((ret = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_I2SPCM_INTERFACE_PARAM,\
                                           p_buf, hw_enable_mSBC_codec_cback)) == FALSE)
                   {
                       bt_vendor_cbacks->dealloc(p_buf);
                   }
               }
            }
            break;

            case HW_WBS_I2S: {
                BTHWDBG("HW_WBS_I2S");
                pthread_mutex_unlock(&lpm_mutex);
                bt_vendor_cbacks->dealloc(p_buf);
            }
            break;
        }
    }
    /* Free the RX event buffer */
    if (bt_vendor_cbacks)
        bt_vendor_cbacks->dealloc(p_evt_buf);
}

/*******************************************************************************
**
** Function         hw_enable_mSBC_codec
**
** Description      Enable mSBC codec
**
** Returns          None
**
*******************************************************************************/
void hw_enable_mSBC_codec(uint8_t state) {
    HC_BT_HDR *p_buf = NULL;
    uint8_t *p, ret;

    if (bt_vendor_cbacks)
            p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                HCI_CMD_PREAMBLE_SIZE + \
                                                MSBC_ENABLE_PARAM_SIZE);
        if (p_buf)
        {
            p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
            p_buf->offset = 0;
            p_buf->layer_specific = 0;

            p = (uint8_t *) (p_buf + 1);
            UINT16_TO_STREAM(p, HCI_VSC_WRITE_MSBC_ENABLE_PARAM);
            if (state == TRUE) {
                p_buf->len = HCI_CMD_PREAMBLE_SIZE + MSBC_ENABLE_PARAM_SIZE;
                *p++ = MSBC_ENABLE_PARAM_SIZE;
                memcpy(p, &msbc_enable_param, MSBC_ENABLE_PARAM_SIZE);
            } else {
                p_buf->len = HCI_CMD_PREAMBLE_SIZE + MSBC_DISABLE_PARAM_SIZE;
                *p++ = MSBC_DISABLE_PARAM_SIZE;
                memcpy(p, &msbc_disable_param, MSBC_DISABLE_PARAM_SIZE);
            }

            if (!pthread_mutex_lock(&lpm_mutex)) {
                hw_wbs_cb_state = HW_WBS_CODEC;
                if ((ret = bt_vendor_cbacks->xmit_cb(HCI_VSC_WRITE_MSBC_ENABLE_PARAM,\
                                p_buf, hw_enable_mSBC_codec_cback)) == FALSE)
                {
                    bt_vendor_cbacks->dealloc(p_buf);
                    pthread_mutex_unlock(&lpm_mutex);
                }
                else
                    return;
            }
        }

    if (bt_vendor_cbacks)
    {
        if (state)
            ALOGE("enable mSBC aborted");
        else
            ALOGE("disable mSBC aborted");
    }
}
#endif // (SCO_USE_I2S_INTERFACE == TRUE)

/*******************************************************************************
**
** Function        hw_set_patch_file_path
**
** Description     Set the location of firmware patch file
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
int hw_set_patch_file_path(char *p_conf_name, char *p_conf_value, int param)
{

    strcpy(fw_patchfile_path, p_conf_value);

    return 0;
}

/*******************************************************************************
**
** Function        hw_set_patch_file_name
**
** Description     Give the specific firmware patch filename
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
int hw_set_patch_file_name(char *p_conf_name, char *p_conf_value, int param)
{

    strcpy(fw_patchfile_name, p_conf_value);

    return 0;
}

#if (VENDOR_LIB_RUNTIME_TUNING_ENABLED == TRUE)
/*******************************************************************************
**
** Function        hw_set_patch_settlement_delay
**
** Description     Give the specific firmware patch settlement time in milliseconds
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
int hw_set_patch_settlement_delay(char *p_conf_name, char *p_conf_value, int param)
{
    fw_patch_settlement_delay = atoi(p_conf_value);

    return 0;
}
#endif  //VENDOR_LIB_RUNTIME_TUNING_ENABLED

static inline int set_param(char *p_name, uint8_t p_value, int param, int size, \
                            const char *param_name[], uint8_t *bt_param)
{
    int i;
    BTHWDBG( "%s: parameter: %s value: %u", __func__, p_name, p_value);

    for (i = 0; i < size; i++) {
         if (strcmp(param_name[i], p_name) == 0) {
           bt_param[i] = p_value;
           return 0;
        }
    }

    // no parameter matching
    ALOGE( "%s: invalid parameter %s", __func__, p_name);
    return -EINVAL;
}

/*******************************************************************************
**
** Function        hw_pcm_set_param
**
** Description     Set SCO PCM parameters
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
int hw_pcm_set_param(char *p_name, char *p_value, int param)
{
    return set_param(p_name, p_value, param, SCO_PCM_PARAM_SIZE, \
                     sco_pcm_parameter_name, bt_pcm_sco_param);
}

/*******************************************************************************
**
** Function        hw_pcm_fmt_set_param
**
** Description     Set SCO PCM Format parameters
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
int hw_pcm_fmt_set_param(char *p_name, uint8_t p_value, int param)
{
    return set_param(p_name, p_value, param, PCM_DATA_FORMAT_PARAM_SIZE, \
                     pcm_data_fmt_parameter_name, bt_pcm_data_fmt_param);
}

#if (SCO_USE_I2S_INTERFACE == TRUE)
/*******************************************************************************
**
** Function        hw_i2s_set_param
**
** Description     Set SCO I2S parameters
**
** Returns         0 : Success
**                 Otherwise : Fail
**
*******************************************************************************/
int hw_i2s_set_param(char *p_name, uint8_t p_value, int param)
{
    return set_param(p_name, p_value, param, SCO_I2SPCM_PARAM_SIZE, \
                     sco_i2s_parameter_name, bt_i2s_sco_param);
}

/*******************************************************************************
**
** Function         hw_wbs_config
**
** Description      Configure WBS related hardware settings
**
** Returns          None
**
*******************************************************************************/
void hw_wbs_enable(uint8_t wbs_state)
{
    if (wbs_state == TRUE) {
        hw_pcm_set_param("SCO_PCM_IF_CLOCK_RATE", SCO_PCM_IF_CLOCK_RATE_WBS, 0);
        hw_i2s_set_param("SCO_I2SPCM_IF_SAMPLE_RATE", SCO_I2SPCM_IF_SAMPLE_RATE_WBS, 0);
        hw_i2s_set_param("SCO_I2SPCM_IF_CLOCK_RATE", SCO_I2SPCM_IF_CLOCK_RATE_WBS, 0);
    } else {
        hw_pcm_set_param("SCO_PCM_IF_CLOCK_RATE", SCO_PCM_IF_CLOCK_RATE, 0);
        hw_i2s_set_param("SCO_I2SPCM_IF_SAMPLE_RATE", SCO_I2SPCM_IF_SAMPLE_RATE, 0);
        hw_i2s_set_param("SCO_I2SPCM_IF_CLOCK_RATE", SCO_I2SPCM_IF_CLOCK_RATE, 0);
    }
    hw_enable_mSBC_codec(wbs_state);
}
#endif // (SCO_USE_I2S_INTERFACE == TRUE)

/*****************************************************************************
**   Sample Codes Section
*****************************************************************************/

#if (HW_END_WITH_HCI_RESET == TRUE)
/*******************************************************************************
**
** Function         hw_epilog_cback
**
** Description      Callback function for Command Complete Events from HCI
**                  commands sent in epilog process.
**
** Returns          None
**
*******************************************************************************/
void hw_epilog_cback(void *p_mem)
{
    HC_BT_HDR   *p_evt_buf = (HC_BT_HDR *) p_mem;
    uint8_t     *p, status;
    uint16_t    opcode;

    status = *((uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_STATUS_RET_BYTE);
    p = (uint8_t *)(p_evt_buf + 1) + HCI_EVT_CMD_CMPL_OPCODE;
    STREAM_TO_UINT16(opcode,p);

    BTHWDBG("%s Opcode:0x%04X Status: %d", __FUNCTION__, opcode, status);

    if (bt_vendor_cbacks)
    {
        /* Must free the RX event buffer */
        bt_vendor_cbacks->dealloc(p_evt_buf);

        /* Once epilog process is done, must call epilog_cb callback
           to notify caller */
        bt_vendor_cbacks->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
    }
}

/*******************************************************************************
**
** Function         hw_epilog_process
**
** Description      Sample implementation of epilog process
**
** Returns          None
**
*******************************************************************************/
void hw_epilog_process(void)
{
    HC_BT_HDR  *p_buf = NULL;
    uint8_t     *p;

    BTHWDBG("hw_epilog_process");

    /* Sending a HCI_RESET */
    if (bt_vendor_cbacks)
    {
        /* Must allocate command buffer via HC's alloc API */
        p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + \
                                                       HCI_CMD_PREAMBLE_SIZE);
    }

    if (p_buf)
    {
        p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
        p_buf->offset = 0;
        p_buf->layer_specific = 0;
        p_buf->len = HCI_CMD_PREAMBLE_SIZE;

        p = (uint8_t *) (p_buf + 1);
        UINT16_TO_STREAM(p, HCI_RESET);
        *p = 0; /* parameter length */

        /* Send command via HC's xmit_cb API */
        bt_vendor_cbacks->xmit_cb(HCI_RESET, p_buf, hw_epilog_cback);
    }
    else
    {
        if (bt_vendor_cbacks)
        {
            ALOGE("vendor lib epilog process aborted [no buffer]");
            bt_vendor_cbacks->epilog_cb(BT_VND_OP_RESULT_FAIL);
        }
    }
}
#endif // (HW_END_WITH_HCI_RESET == TRUE)
