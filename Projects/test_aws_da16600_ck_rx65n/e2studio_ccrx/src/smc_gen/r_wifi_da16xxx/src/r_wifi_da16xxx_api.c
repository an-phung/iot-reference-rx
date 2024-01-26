/**********************************************************************************************************************
 * DISCLAIMER
 * This software is supplied by Renesas Electronics Corporation and is only intended for use with Renesas products. No
 * other uses are authorized. This software is owned by Renesas Electronics Corporation and is protected under all
 * applicable laws, including copyright laws.
 * THIS SOFTWARE IS PROVIDED "AS IS" AND RENESAS MAKES NO WARRANTIES REGARDING
 * THIS SOFTWARE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. ALL SUCH WARRANTIES ARE EXPRESSLY DISCLAIMED. TO THE MAXIMUM
 * EXTENT PERMITTED NOT PROHIBITED BY LAW, NEITHER RENESAS ELECTRONICS CORPORATION NOR ANY OF ITS AFFILIATED COMPANIES
 * SHALL BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES FOR ANY REASON RELATED TO
 * THIS SOFTWARE, EVEN IF RENESAS OR ITS AFFILIATES HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * Renesas reserves the right, without notice, to make changes to this software and to discontinue the availability of
 * this software. By using this software, you agree to the additional terms and conditions found by accessing the
 * following link:
 * http://www.renesas.com/disclaimer
 *
 * Copyright (C) 2023 Renesas Electronics Corporation. All rights reserved.
 *********************************************************************************************************************/
/**********************************************************************************************************************
 * File Name    : r_wifi_da16xxx_api.c
 * Description  : API functions definition for DA16XXX.
 *********************************************************************************************************************/

/**********************************************************************************************************************
 Includes   <System Includes> , "Project Includes"
 *********************************************************************************************************************/
#include "r_wifi_da16xxx_private.h"

/**********************************************************************************************************************
 Macro definitions
 *********************************************************************************************************************/
#define UART_BAUD_MAX_CNT    (4)

/**********************************************************************************************************************
 Local Typedef definitions
 *********************************************************************************************************************/

/**********************************************************************************************************************
 Exported global variables
 *********************************************************************************************************************/
st_uart_tbl_t g_uart_tbl;
st_sock_tbl_t g_sock_tbl[SOCK_TBL_MAX];
static uint8_t g_rx_buff[TEMP_BUf_MAX];
static uint8_t g_sock_list[SOCK_TBL_MAX + 2]; //number of sockets + 2 others socket type
volatile uint8_t g_cur_sock_idx = UINT8_MAX;
volatile uint8_t g_rx_idx = 0;

/**********************************************************************************************************************
 Private (static) variables and functions
 *********************************************************************************************************************/
/* System state control */
static void wifi_system_state_set (e_wifi_module_status_t state);
static e_wifi_module_status_t wifi_system_state_get (void);

/* Sub functions */
static wifi_err_t disconnect_ap_sub (void);
static void da16xxx_handle_incoming_socket_data(uint8_t data);

/* BYTEQ control for socket */
static wifi_err_t socket_byteq_open (void);
static void socket_byteq_close (void);

/* WIFI module control */
static void da16xxx_hw_reset (void);
static void da16xxx_close (void);
#if WIFI_CFG_SNTP_ENABLE == 1
static wifi_err_t da16xxx_sntp_service_init(void);
#endif

/* SCI callback functions for UART */
static void cb_sci_uart (void * pArgs);
static void cb_sci_err (sci_cb_evt_t event);

/* Log functions */
static int s_vsnprintf_safe (char * s, size_t n, const char * format, va_list arg);
static int s_snprintf_safe (char * s, size_t n, const char * format, ...);
static void s_log_printf_common (uint8_t usLoggingLevel, const char * pcFormat, va_list args);

/* WIFI system state */
static volatile e_wifi_module_status_t s_wifi_system_state = MODULE_DISCONNECTED;

/* IP address */
static wifi_ip_configuration_t s_cur = {0};  /* Current IP configuration of the module */

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_Open
 * Description  : Open WIFI Module.
 * Arguments    : none
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_ALREADY_OPEN
 *                WIFI_ERR_SERIAL_OPEN
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_BYTEQ_OPEN
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_Open(void)
{
    sci_baud_t change_baud;
    wifi_err_t api_ret = WIFI_SUCCESS;
    uint8_t index = 0;
    uint32_t uart_baud_rates[UART_BAUD_MAX_CNT] = {115200, 230400, 460800, 921600};

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED != wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Open is called before!"));
        return WIFI_ERR_ALREADY_OPEN;
    }

    /* Memory initialize */
    memset(&g_uart_tbl, 0, sizeof(g_uart_tbl));
    memset(g_sock_tbl, 0, sizeof(g_sock_tbl));
    at_resp_byteq_open();

    /* Mutex initialize */
    if (WIFI_SUCCESS != os_wrap_mutex_create())
    {
        api_ret = WIFI_ERR_TAKE_MUTEX;
        goto END_INITIALIZE;
    }

    /* Reset WIFI module */
    da16xxx_hw_reset();

    /* Open serial port */
    if (ATCMD_OK != uart_port_open(cb_sci_uart))
    {
        api_ret = WIFI_ERR_SERIAL_OPEN;
        goto END_INITIALIZE;
    }

    for (index = 0; index < UART_BAUD_MAX_CNT; index++)
    {
    	/* Set baud rate for serial port */
        change_baud.pclk = WIFI_CFG_SCI_PCLK_HZ;
        change_baud.rate = uart_baud_rates[index];
        R_SCI_Control(g_uart_tbl.sci_hdl, SCI_CMD_CHANGE_BAUD, &change_baud);

        os_wrap_sleep(10, UNIT_MSEC);

        /* Test basic communications with an AT command. */
        WIFI_LOG_INFO(("R_WIFI_DA16XXX_Open: Test with baud rate %d!", uart_baud_rates[index]));
        if (AT_OK != at_exec("ATZ\r"))
        {
#if (WIFI_CFG_DA16600_SUPPORT)
            os_wrap_sleep(5000, UNIT_MSEC);
#else
            os_wrap_sleep(10, UNIT_MSEC);
#endif
            if (AT_OK != at_exec("ATZ\r"))
            {
                os_wrap_sleep(10, UNIT_MSEC);
            }
            else
            {
                break;
            }
        }
        else
        {
            break;
        }
    }

    if (index == UART_BAUD_MAX_CNT)
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Open: Cannot communicate with Wi-fi module!"));
        api_ret = WIFI_ERR_MODULE_COM;
        goto END_INITIALIZE;
    }

    /* Update the module baud rate in case if it doesn't match with user configured baud rate */
    WIFI_LOG_INFO(("R_WIFI_DA16XXX_Open: baud rate:%d", WIFI_CFG_SCI_BAUDRATE));
    if (uart_baud_rates[index] != WIFI_CFG_SCI_BAUDRATE)
    {
        /* Configure UART parameters for UART port. */
        if (AT_OK != at_exec("ATB=%d,8,n,1,%d\r", WIFI_CFG_SCI_BAUDRATE, WIFI_CFG_CTS_SW_CTRL))
        {
            WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Open: Cannot update Wi-fi module baud rate!"));
            api_ret = WIFI_ERR_MODULE_COM;
            goto END_INITIALIZE;
        }

        /* Set baud rate for serial port */
        change_baud.pclk = WIFI_CFG_SCI_PCLK_HZ;
        change_baud.rate = WIFI_CFG_SCI_BAUDRATE;
        R_SCI_Control(g_uart_tbl.sci_hdl, SCI_CMD_CHANGE_BAUD, &change_baud);

        os_wrap_sleep(10, UNIT_MSEC);
    }

    /* Show version info */
    at_exec("AT+VER\r");

#if WIFI_CFG_DEBUG_LOG == 4
    /* Enable echo back */
    at_exec("ATE\r");
#endif

    /* Set AP mode */
    at_exec("AT+WFMODE=0\r");

    /* Set Country Code */
    at_exec("AT+WFCC=%s\r", WIFI_CFG_COUNTRY_CODE);

    /* Set WIFI State to "Connected WiFi module" */
    wifi_system_state_set(MODULE_CONNECTED);
#if WIFI_CFG_SNTP_ENABLE == 1
    api_ret = da16xxx_sntp_service_init();
    if (WIFI_SUCCESS != api_ret)
    {
        goto END_INITIALIZE;
    }
#endif

    /* Initialize BYTEQ on socket table */
    if (WIFI_SUCCESS != socket_byteq_open())
    {
        api_ret = WIFI_ERR_BYTEQ_OPEN;
        goto END_INITIALIZE;
    }
    memset(g_sock_list, UINT8_MAX, sizeof(g_sock_list));

    /* Set flow control for serial port */
    flow_ctrl_init();

END_INITIALIZE:
    if (WIFI_SUCCESS != api_ret)
    {
        da16xxx_close();
    }
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_Open
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_Close
 * Description  : Close WIFI Module.
 * Arguments    : none
 * Return Value : WIFI_SUCCESS
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_Close(void)
{
    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        return WIFI_SUCCESS;
    }

    disconnect_ap_sub();

    /* Close module */
    da16xxx_close();

    return WIFI_SUCCESS;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_Close
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_Ping
 * Description  : Execute Ping command.
 * Arguments    : ip_address
 *                count
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_Ping(uint8_t *ip_address, uint16_t count)
{
    wifi_err_t api_ret = WIFI_SUCCESS;
    int sent_count = 0;
    int recv_count = 0;

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Ping: R_WIFI_DA16XXX_Connect is not called!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if ((NULL == ip_address) || (0 == count))
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    /* Send ping to specified IP address. */
    at_set_timeout(5000 * count);
    if (AT_OK == at_exec_wo_mutex("AT+NWPING=0,%d.%d.%d.%d,%d\r",
                                  ip_address[0], ip_address[1], ip_address[2], ip_address[3], count))
    {
        at_read("+NWPING:%d,%d", &sent_count, &recv_count);
        if (sent_count != recv_count)
        {
            WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Ping: Cannot reach to IP address (sent: %d, recv: %d)!",
                            sent_count, recv_count));
            api_ret = WIFI_ERR_MODULE_COM;
        }
    }
    else
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }
    at_set_timeout(ATCMD_RESP_TIMEOUT);

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_Ping
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_Scan
 * Description  : Scan Access points.
 * Arguments    : ap_results
 *                max_networks
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_Scan(wifi_scan_result_t * ap_results, uint32_t max_networks)
{
    wifi_err_t     api_ret = WIFI_SUCCESS;
    e_rslt_code_t  at_rslt;
    uint32_t       data_ret = DATA_NOT_FOUND;
    uint32_t       i;
    uint8_t        ssid_tmp[WIFI_CFG_MAX_SSID_LEN] = "\0";
    int            bssid_tmp[WIFI_CFG_MAX_BSSID_LEN] = {0};
    int            rssi_tmp = 0;
    uint8_t        flag[50] = "\0";

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Scan: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Check parameters */
    if (NULL == ap_results)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    /* Show AP scan result. */
    at_set_timeout(60000);
    at_rslt = at_exec_wo_mutex("AT+WFSCAN\r");
    if (AT_OK != at_rslt)
    {
        api_ret = WIFI_ERR_MODULE_COM;
        goto RETURN_ERROR;
    }

    /* set timeout to default */
    at_set_timeout(ATCMD_RESP_TIMEOUT);

    /* clear tmp variables */
    memset(ssid_tmp, 0, WIFI_CFG_MAX_SSID_LEN);
    memset(flag, 0, 50);
    i = 0;
    while (i != max_networks)
    {
        if (0 == strncmp((const char *)at_get_current_line(), "OK\r\n", 4))
        {
            break;
        }
        if (i == 0)
        {
            data_ret = at_read("+WFSCAN:%x:%x:%x:%x:%x:%x\t%*d\t%d\t%s%*[\t]%[^\n]\n",
                               &bssid_tmp[0], &bssid_tmp[1], &bssid_tmp[2],
                               &bssid_tmp[3], &bssid_tmp[4], &bssid_tmp[5],
                               &rssi_tmp, flag, ssid_tmp);
        }
        else
        {
            data_ret = at_read_wo_prefix("%x:%x:%x:%x:%x:%x\t%*d\t%d\t%s%*[\t]%[^\n]\n",
                                         &bssid_tmp[0], &bssid_tmp[1], &bssid_tmp[2],
                                         &bssid_tmp[3], &bssid_tmp[4], &bssid_tmp[5],
                                         &rssi_tmp, flag, ssid_tmp);
            at_move_to_next_line();
        }
        if (DATA_FOUND == data_ret)
        {
            /* bssi */
            ap_results[i].bssid[0] = (uint8_t) bssid_tmp[0];
            ap_results[i].bssid[1] = (uint8_t) bssid_tmp[1];
            ap_results[i].bssid[2] = (uint8_t) bssid_tmp[2];
            ap_results[i].bssid[3] = (uint8_t) bssid_tmp[3];
            ap_results[i].bssid[4] = (uint8_t) bssid_tmp[4];
            ap_results[i].bssid[5] = (uint8_t) bssid_tmp[5];

            /* signal strength */
            ap_results[i].rssi = (int8_t)(rssi_tmp);

            /* security */
            if (NULL != strstr((const char *)flag, "[WPA2-PSK"))
            {
                ap_results[i].security = WIFI_SECURITY_WPA2;
            }
            else if (NULL != strstr((const char *)flag, "[WPA-PSK"))
            {
                ap_results[i].security = WIFI_SECURITY_WPA;
            }
            else if (NULL != strstr((const char *)flag, "[WPS]"))
            {
                ap_results[i].security = WIFI_SECURITY_UNDEFINED;
            }
            else
            {
                ap_results[i].security = WIFI_SECURITY_OPEN;
            }

            /* encryption */
            if (NULL != strstr((const char *)flag, "CCMP+TKIP"))
            {
                ap_results[i].encryption = WIFI_ENCRYPTION_TKIP_AES;
            }
            else if (NULL != strstr((const char *)flag, "CCMP"))
            {
                ap_results[i].encryption = WIFI_ENCRYPTION_AES;
            }
            else if (NULL != strstr((const char *)flag, "TKIP"))
            {
                ap_results[i].encryption = WIFI_ENCRYPTION_TKIP;
            }
            else
            {
                ap_results[i].encryption = WIFI_ENCRYPTION_UNDEFINED;
            }

            /* ssid */
            WIFI_LOG_INFO(("R_WIFI_DA16XXX_Scan: AP result:"));
            if (ssid_tmp[0] == '\0')
            {
                ap_results[i].hidden = 1;
            }
            else
            {
                ap_results[i].hidden = 0;
                memcpy(ap_results[i].ssid, ssid_tmp, sizeof(ssid_tmp));
                WIFI_LOG_INFO(("SSID: %s", ap_results[i].ssid));
            }
            WIFI_LOG_INFO(("BSSID: %X.%X.%X.%X.%X.%X", bssid_tmp[0],
                                                       bssid_tmp[1],
                                                       bssid_tmp[2],
                                                       bssid_tmp[3],
                                                       bssid_tmp[4],
                                                       bssid_tmp[5]));
            WIFI_LOG_INFO(("Security: %d", ap_results[i].security));
            WIFI_LOG_INFO(("Encryption: %d", ap_results[i].encryption));

            /* clear tmp variables */
            memset(ssid_tmp, 0, WIFI_CFG_MAX_SSID_LEN);
            memset(flag, 0, 50);
            i++;
        }
    }

RETURN_ERROR:
    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_Scan
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_Connect
 * Description  : Connect to Access Point.
 * Arguments    : ssid
 *                pass
 *                security
 *                enc_type
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_Connect(const uint8_t * ssid, const uint8_t * pass,
        wifi_security_t security, wifi_encryption_t enc_type)
{
    uint8_t retry_count;
    uint8_t retry_max = 3;
    wifi_err_t api_ret = WIFI_SUCCESS;
    uint8_t at_cmd[WIFI_CFG_MAX_SSID_LEN + WIFI_CFG_MAX_PASS_LEN + 20];

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Connect: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Already connected access point? */
    if (0 == R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Connect: Already connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check ssid */
    if (NULL == ssid)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Connect to an open security AP */
    if (WIFI_SECURITY_OPEN == security)
    {
        snprintf((char *) at_cmd, sizeof(at_cmd), "AT+WFJAP='%s',%d\r", ssid, security);
    }
    else if (WIFI_SECURITY_WPA == security || WIFI_SECURITY_WPA2 == security)
    {
        snprintf((char *) at_cmd, sizeof(at_cmd), "AT+WFJAP='%s',%d,%d,'%s'\r", ssid, security, enc_type, pass);
    }
    else if (WIFI_SECURITY_WPA3 == security)
    {
        snprintf((char *) at_cmd, sizeof(at_cmd), "AT+WFJAPA='%s','%s'\r", ssid, pass);
    }
    else
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Connect: Unsupported security type (%d)!", security));
        api_ret = WIFI_ERR_PARAMETER;
        goto RETURN_ERROR;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    /* Call to get WiFi connection does not always work the first time */
    at_set_timeout(30000);
    api_ret = WIFI_ERR_MODULE_COM;
    for (retry_count = 0; retry_count < retry_max; retry_count++)
    {
        if (AT_OK == at_exec_wo_mutex((char *) at_cmd))
        {
            if (AT_OK == at_exec_wo_mutex("AT+WFSTA\r"))
            {
                api_ret = WIFI_SUCCESS;
                break;
            }
        }
        os_wrap_sleep(1000, UNIT_MSEC);
    }
    at_set_timeout(ATCMD_RESP_TIMEOUT);

    /* flush SSID, password AP information */
    memset(at_cmd, 0, sizeof(at_cmd));

    if (api_ret == WIFI_SUCCESS)
    {
        if (DATA_FOUND == at_read("+WFSTA:1\r"))
        {
            /* Set WIFI State to "Connected access point" */
            wifi_system_state_set(MODULE_ACCESSPOINT);

            /* DHCP setting */
            if (AT_OK != at_exec_wo_mutex("AT+NWDHC=1\r"))
            {
                api_ret = WIFI_ERR_MODULE_COM;
                goto GIVE_MUTEX;
            }

            /* Give mutex */
            os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

            /* Get IP address */
            api_ret = R_WIFI_DA16XXX_GetIpAddress(&s_cur);
            return api_ret;
        }
        else
        {
            WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Connect: Cannot connect to access point!"));
            api_ret = WIFI_ERR_NOT_CONNECT;
        }
    }

GIVE_MUTEX:
    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

RETURN_ERROR:
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_Connect
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_Disconnect
 * Description  : Disconnect from Access Point.
 * Arguments    : none
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_Disconnect(void)
{
    wifi_err_t api_ret = WIFI_SUCCESS;

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_Disconnect: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    if (WIFI_SUCCESS != disconnect_ap_sub())
    {
        api_ret = WIFI_ERR_MODULE_COM;
        goto RETURN_ERROR;
    }

    /* Set WIFI State to "Connected WiFi module" */
    wifi_system_state_set(MODULE_CONNECTED);

RETURN_ERROR:
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_Disconnect
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_IsConnected
 * Description  : Check connected access point.
 * Arguments    : none
 * Return Value : 0  - connected
 *                -1 - disconnected
 *********************************************************************************************************************/
int32_t R_WIFI_DA16XXX_IsConnected(void)
{
    /* Connected access point? */
    if (MODULE_ACCESSPOINT == wifi_system_state_get())
    {
        return 0;
    }
    else
    {
        return (-1);
    }
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_IsConnected
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_DnsQuery
 * Description  : Execute DNS query.
 * Arguments    : domain_name
 *                ip_address
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_DnsQuery(uint8_t * domain_name, uint8_t * ip_address)
{
    wifi_err_t api_ret = WIFI_ERR_MODULE_COM;
    int        ip_tmp[4];

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_DnsQuery: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if ((NULL == domain_name) || (NULL == ip_address))
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    at_set_timeout(10000);
    if (AT_OK == at_exec_wo_mutex("AT+NWHOST=%s\r", domain_name))
    {
        if (DATA_FOUND == at_read("+NWHOST:%d.%d.%d.%d",
                                  &ip_tmp[0], &ip_tmp[1], &ip_tmp[2], &ip_tmp[3]))
        {
            ip_address[0] = (uint8_t) ip_tmp[0];
            ip_address[1] = (uint8_t) ip_tmp[1];
            ip_address[2] = (uint8_t) ip_tmp[2];
            ip_address[3] = (uint8_t) ip_tmp[3];
            api_ret = WIFI_SUCCESS;
        }
    }
    at_set_timeout(ATCMD_RESP_TIMEOUT);

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_DnsQuery
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_SntpServerIpAddressSet
 * Description  : Set SNTP server IP address.
 * Arguments    : ip_address
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_SntpServerIpAddressSet (uint8_t * ip_address)
{
    wifi_err_t api_ret = WIFI_SUCCESS;

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_SntpServerIpAddressSet: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Check parameters */
    if (NULL == ip_address)
    {
        return WIFI_ERR_PARAMETER;
    }

    if (AT_OK != at_exec("AT+NWSNS=%d.%d.%d.%d\r", ip_address[0], ip_address[1], ip_address[2], ip_address[3]))
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_SntpServerIpAddressSet
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_SntpEnableSet
 * Description  : Enable or disable SNTP client service.
 * Arguments    : enable
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_SntpEnableSet (wifi_sntp_enable_t enable)
{
    wifi_err_t api_ret = WIFI_SUCCESS;

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_SntpEnableSet: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Check parameters */
    if (enable != WIFI_SNTP_ENABLE && enable != WIFI_SNTP_DISABLE)
    {
        return WIFI_ERR_PARAMETER;
    }

    if (AT_OK != at_exec("AT+NWSNTP=%d\r", enable))
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_SntpEnableSet
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_SntpTimeZoneSet
 * Description  : Set SNTP timezone.
 * Arguments    : utc_offset_in_hours
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_SntpTimeZoneSet (int utc_offset_in_hours)
{
    wifi_err_t api_ret = WIFI_SUCCESS;

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_SntpTimeZoneSet: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Check parameters */
    if (utc_offset_in_hours < -12 || utc_offset_in_hours > 12)
    {
        return WIFI_ERR_PARAMETER;
    }

    if (AT_OK != at_exec("AT+TZONE=%d\r", utc_offset_in_hours * 3600))
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_SntpTimeZoneSet
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_LocalTimeGet
 * Description  : Get the current local time based on current timezone in a string . Exp: YYYY-MM-DD,HOUR:MIN:SECS.
 * Arguments    : local_time
 *                size_string
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_OPEN
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_LocalTimeGet (uint8_t * local_time, uint32_t size_string)
{
    wifi_err_t api_ret = WIFI_SUCCESS;

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_LocalTimeGet: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Check parameters */
    if (NULL == local_time || 20 > size_string)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    if (AT_OK == at_exec_wo_mutex("AT+TIME=?\r"))
    {
        if (DATA_NOT_FOUND == at_read("+TIME:%s", local_time))
        {
            api_ret = WIFI_ERR_MODULE_COM;
        }
    }
    else
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_LocalTimeGet
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_SetDnsServerAddress
 * Description  : Set DNS Server Address.
 * Arguments    : dns_address
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_SetDnsServerAddress (uint8_t * dns_address)
{
    wifi_err_t api_ret = WIFI_SUCCESS;

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_SetDnsServerAddress: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (NULL == dns_address)
    {
        return WIFI_ERR_PARAMETER;
    }


    if (AT_OK != at_exec("AT+NWDNS=%d.%d.%d.%d\r", dns_address[0], dns_address[1], dns_address[2], dns_address[3]))
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_SetDnsServerAddress
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_GetMacAddress
 * Description  : Get MAC Address.
 * Arguments    : mac_address.
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_GetMacAddress (uint8_t * mac_address)
{
    wifi_err_t api_ret = WIFI_ERR_MODULE_COM;
    int        mac_tmp[6];

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_GetMacAddress: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (NULL == mac_address)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    if (AT_OK == at_exec_wo_mutex("AT+WFMAC=?\r"))
    {
        if (DATA_FOUND == at_read("+WFMAC:%X:%X:%X:%X:%X:%X",
                                  &mac_tmp[0], &mac_tmp[1], &mac_tmp[2],
                                  &mac_tmp[3], &mac_tmp[4], &mac_tmp[5]))
        {
            mac_address[0] = (uint8_t) mac_tmp[0];
            mac_address[1] = (uint8_t) mac_tmp[1];
            mac_address[2] = (uint8_t) mac_tmp[2];
            mac_address[3] = (uint8_t) mac_tmp[3];
            mac_address[4] = (uint8_t) mac_tmp[4];
            mac_address[5] = (uint8_t) mac_tmp[5];
            api_ret = WIFI_SUCCESS;
        }
    }

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_GetMacAddress
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_GetIpAddress
 * Description  : Get IP Address.
 * Arguments    : ip_config.
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_GetIpAddress (wifi_ip_configuration_t * ip_config)
{
    wifi_err_t api_ret = WIFI_ERR_MODULE_COM;
    int        ip_tmp[4];
    int        sub_tmp[4];
    int        gw_tmp[4];

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_GetIpAddress: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (NULL == ip_config)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    if (AT_OK == at_exec_wo_mutex("AT+NWIP=?\r"))
    {
        if (DATA_FOUND == at_read("+NWIP:0,%d.%d.%d.%d,%d.%d.%d.%d,%d.%d.%d.%d",
                                  &ip_tmp[0], &ip_tmp[1], &ip_tmp[2], &ip_tmp[3],
                                  &sub_tmp[0], &sub_tmp[1], &sub_tmp[2], &sub_tmp[3],
                                  &gw_tmp[0], &gw_tmp[1], &gw_tmp[2], &gw_tmp[3]))
        {
            ip_config->ipaddress[0] = (uint8_t) ip_tmp[0];
            ip_config->ipaddress[1] = (uint8_t) ip_tmp[1];
            ip_config->ipaddress[2] = (uint8_t) ip_tmp[2];
            ip_config->ipaddress[3] = (uint8_t) ip_tmp[3];
            ip_config->subnetmask[0] = (uint8_t) sub_tmp[0];
            ip_config->subnetmask[1] = (uint8_t) sub_tmp[1];
            ip_config->subnetmask[2] = (uint8_t) sub_tmp[2];
            ip_config->subnetmask[3] = (uint8_t) sub_tmp[3];
            ip_config->gateway[0] = (uint8_t) gw_tmp[0];
            ip_config->gateway[1] = (uint8_t) gw_tmp[1];
            ip_config->gateway[2] = (uint8_t) gw_tmp[2];
            ip_config->gateway[3] = (uint8_t) gw_tmp[3];
            api_ret = WIFI_SUCCESS;
        }
    }

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_GetIpAddress
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_GetAvailableSocket
 * Description  : Get the next available socket ID.
 * Arguments    : socket_id
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_NUM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_GetAvailableSocket (uint32_t * socket_id)
{
    uint32_t i = 0;

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (NULL == socket_id)
    {
        return WIFI_ERR_PARAMETER;
    }

    for (i = 0; i < SOCK_TBL_MAX; i++)
    {
        if (g_sock_tbl[i].status == DA16XXX_SOCKET_STATUS_CLOSED)
        {
            *socket_id = i;
            return WIFI_SUCCESS;
        }
    }

    *socket_id = UINT32_MAX;
    return WIFI_ERR_SOCKET_NUM;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_GetAvailableSocket
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_GetSocketStatus
 * Description  : Get the socket status.
 * Arguments    : socket_number
 *                socket_status
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_NUM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_GetSocketStatus (uint32_t socket_number, da16xxx_socket_status_t *socket_status)
{
    wifi_err_t api_ret = WIFI_SUCCESS;
    e_rslt_code_t at_ret = AT_OK;
    uint8_t cid = 0;

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_GetSocketStatus: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (socket_number >= SOCK_TBL_MAX)
    {
        return WIFI_ERR_SOCKET_NUM;
    }
    
    /* get CID number */
    for (cid = g_sock_tbl[socket_number].type; cid < SOCK_TBL_MAX + 2; cid++)
    {
        if (g_sock_list[cid] == socket_number)
        {
            break;
        }
    }
    
    if (cid == UINT8_MAX)
    {
        return WIFI_ERR_SOCKET_NUM;
    }

    at_ret = at_exec("AT+TRPRT=%d\r", cid);
    if (AT_NO_RESULT == at_ret)
    {
        WIFI_LOG_INFO(("R_WIFI_DA16XXX_GetSocketStatus: socket %d is closed!", socket_number));
        g_sock_tbl[socket_number].status = DA16XXX_SOCKET_STATUS_CLOSED;
        api_ret = WIFI_SUCCESS;
    }
    else if (AT_OK != at_ret)
    {
        api_ret = WIFI_ERR_MODULE_COM;
    }

    *socket_status = g_sock_tbl[socket_number].status;

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_GetSocketStatus
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_CreateSocket
 * Description  : Create a new socket instance.
 * Arguments    : socket_number
 *                type
 *                ip_version
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_CREATE
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_CreateSocket (uint32_t socket_number, da16xxx_socket_type_t type, uint8_t ip_version)
{
    wifi_err_t ret = WIFI_ERR_SOCKET_CREATE;

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_CreateSocket: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (socket_number > SOCK_TBL_MAX || DA16XXX_SOCKET_TYPE_TCP_CLIENT != type)
    {
        return WIFI_ERR_PARAMETER;
    }

    if (DA16XXX_SOCKET_STATUS_CLOSED == g_sock_tbl[socket_number].status)
    {
        WIFI_LOG_INFO(("R_WIFI_DA16XXX_CreateSocket: Creating socket %d!", socket_number));
        g_sock_tbl[socket_number].status = DA16XXX_SOCKET_STATUS_SOCKET;    /* socket status   */
        g_sock_tbl[socket_number].ipver = ip_version;                       /* ip_version      */
        g_sock_tbl[socket_number].type = type;                              /* type            */
        g_sock_tbl[socket_number].put_err_cnt = 0;
        R_BYTEQ_Flush(g_sock_tbl[socket_number].byteq_hdl);
        ret = WIFI_SUCCESS;
    }
    else
    {
        WIFI_LOG_WARN(("R_WIFI_DA16XXX_CreateSocket: socket %d has already created!", socket_number));
    }

    /* Clear the buffer */
    memset(g_uart_tbl.recv_buf, 0, sizeof(g_uart_tbl.recv_buf));

    return ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_CreateSocket
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_TcpConnect
 * Description  : Connect to a specific IP and Port using socket.
 * Arguments    : socket_number
 *                ip_address
 *                port
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_NUM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_TcpConnect (uint32_t socket_number, uint8_t * ip_address, uint16_t port)
{
    wifi_err_t  api_ret = WIFI_ERR_MODULE_COM;
    uint8_t     cid = 0;

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_TcpConnect: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if (socket_number > SOCK_TBL_MAX || NULL == ip_address || 0 == port)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* socket created? */
    if (DA16XXX_SOCKET_STATUS_SOCKET != g_sock_tbl[socket_number].status)
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_TcpConnect: socket %d is not created!", socket_number));
        return WIFI_ERR_SOCKET_NUM;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX | MUTEX_RX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    at_set_timeout(10000);
    if (AT_OK == at_exec_wo_mutex("AT+TRTC=%d.%d.%d.%d,%d,0\r",
                                  ip_address[0], ip_address[1], ip_address[2], ip_address[3], port))
    {
        if (DATA_FOUND == at_read("+TRTC:%d", &cid))
        {
            g_sock_tbl[socket_number].ipaddr[0] = ip_address[0];
            g_sock_tbl[socket_number].ipaddr[1] = ip_address[1];
            g_sock_tbl[socket_number].ipaddr[2] = ip_address[2];
            g_sock_tbl[socket_number].ipaddr[3] = ip_address[3];
            g_sock_tbl[socket_number].port = port;
            g_sock_tbl[socket_number].status = DA16XXX_SOCKET_STATUS_CONNECTED;
            g_sock_list[cid] = socket_number;
            api_ret = WIFI_SUCCESS;
            WIFI_LOG_INFO(("R_WIFI_DA16XXX_TcpConnect: connected socket %d to TCP server.", socket_number));
        }
    }

    at_set_timeout(ATCMD_RESP_TIMEOUT);

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX | MUTEX_RX);

    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_TcpConnect
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_SendSocket
 * Description  : Send data on connecting socket.
 * Arguments    : socket_number
 *                data
 *                length
 *                timeout_ms
 * Return Value : Positive number - number of sent data
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_NUM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
int32_t R_WIFI_DA16XXX_SendSocket (uint32_t socket_number, uint8_t * data, uint32_t length, uint32_t timeout_ms)
{
    uint32_t    send_idx = 0;
    char        send_data[DA16XXX_AT_CMD_BUF_MAX] = {0};
    int32_t     send_length;
    uint8_t     cid = 0;
    int32_t     api_ret = 0;
    e_atcmd_err_t at_ret = ATCMD_OK;
    e_rslt_code_t rslt_ret = AT_OK;
    OS_TICK     tick_tmp;
    uint32_t    tx_length;

    /* Connect access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_SendSocket: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if ((socket_number > SOCK_TBL_MAX) || (NULL == data))
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Not connect? */
    if (DA16XXX_SOCKET_STATUS_CONNECTED != g_sock_tbl[socket_number].status)
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_SendSocket: socket #%d is not connected!", socket_number));
        return WIFI_ERR_SOCKET_NUM;
    }
    
    /* get CID number */
    for (cid = g_sock_tbl[socket_number].type; cid < SOCK_TBL_MAX + 2; cid++)
    {
        if (g_sock_list[cid] == socket_number)
        {
            break;
        }
    }
    if (cid == SOCK_TBL_MAX + 2)
    {
        return WIFI_ERR_PARAMETER;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX))
    {
        return WIFI_ERR_TAKE_MUTEX;
    }

    /* Set timeout */
    at_set_timeout(timeout_ms);
    tick_tmp = os_wrap_tickcount_get();
    while (send_idx < length)
    {
        /* get length of data sent */
        if (length - send_idx > (uint32_t)(DA16XXX_AT_SOCK_TX_MAX))
        {
            tx_length = (uint32_t)(DA16XXX_AT_SOCK_TX_MAX);
        }
        else
        {
            tx_length = length - send_idx;
        }
        /* get prefix length */
#if WIFI_CFG_SCI_BAUDRATE == 115200
        send_length = snprintf(send_data, DA16XXX_AT_CMD_BUF_MAX, "\x1BS%d%ld,%d.%d.%d.%d,%ld,%s,",
                               cid, (int) tx_length,
                               g_sock_tbl[socket_number].ipaddr[0],
                               g_sock_tbl[socket_number].ipaddr[1],
                               g_sock_tbl[socket_number].ipaddr[2],
                               g_sock_tbl[socket_number].ipaddr[3],
                               g_sock_tbl[socket_number].port, "r");

        WIFI_LOG_DEBUG(("SendSocket: %s", send_data));
        if (ATCMD_OK != at_send_raw((uint8_t *) send_data, (uint32_t) send_length))
        {
            WIFI_LOG_ERROR(("SendSocket: at_send_raw() failed (ret=%d)!", at_ret));
            api_ret = (int32_t) WIFI_ERR_MODULE_COM;
            break;
        }
#else
        rslt_ret = at_exec_wo_mutex("\x1BH%d,%ld,%d.%d.%d.%d,%ld\r",
                                    cid, (int) tx_length,
                                    g_sock_tbl[socket_number].ipaddr[0],
                                    g_sock_tbl[socket_number].ipaddr[1],
                                    g_sock_tbl[socket_number].ipaddr[2],
                                    g_sock_tbl[socket_number].ipaddr[3],
                                    g_sock_tbl[socket_number].port);
        if (AT_OK != rslt_ret)
        {
            WIFI_LOG_ERROR(("SendSocket: at_exec() failed (ret=%d)!", rslt_ret));
            api_ret = (int32_t) WIFI_ERR_MODULE_COM;
            break;
        }
#endif

        if (ATCMD_OK != at_send_raw((uint8_t *) data + send_idx, tx_length))
        {
            WIFI_LOG_ERROR(("SendSocket: at_send_raw() failed (ret=%d)!", at_ret));
            api_ret = (int32_t) WIFI_ERR_MODULE_COM;
            break;
        }
        rslt_ret = at_recv();
        if (AT_OK != rslt_ret)
        {
            WIFI_LOG_ERROR(("SendSocket: at_recv() failed (ret=%d)!", rslt_ret));
            api_ret = (int32_t) WIFI_ERR_MODULE_COM;
            break;
        }

        send_idx += tx_length;
    }
    if (api_ret >= WIFI_SUCCESS)
    {
        api_ret = send_idx;
    }

    at_set_timeout(ATCMD_RESP_TIMEOUT);
    os_wrap_mutex_give(MUTEX_TX);
    WIFI_LOG_INFO(("R_WIFI_DA16XXX_SendSocket: socket %d ret=%d (%d).", socket_number, api_ret, os_wrap_tickcount_get() - tick_tmp));
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_SendSocket
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_ReceiveSocket
 * Description  : Receive data on connecting socket.
 * Arguments    : socket_number
 *                data
 *                length
 *                timeout_ms
 * Return Value : Positive number - number of received data
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_NUM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
int32_t R_WIFI_DA16XXX_ReceiveSocket (uint32_t socket_number, uint8_t * data, uint32_t length, uint32_t timeout_ms)
{
    int32_t     api_ret = WIFI_SUCCESS;
    uint32_t    recv_cnt = 0;
    byteq_err_t byteq_ret;
    OS_TICK     tick_tmp = 0;

    /* Connect access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_ReceiveSocket: Not connected to access point!"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Check parameters */
    if ((socket_number > SOCK_TBL_MAX) || (NULL == data))
    {
        return WIFI_ERR_PARAMETER;
    }

    g_sock_tbl[socket_number].timer_rx.threshold = OS_WRAP_MS_TO_TICKS(timeout_ms);
    if (0 < timeout_ms)
    {
        g_sock_tbl[socket_number].timer_rx.tick_sta = os_wrap_tickcount_get();
    }
    while (recv_cnt < length)
    {
#if defined(__CCRX__) || defined(__ICCRX__) || defined (__RX__)
        R_BSP_InterruptsDisable();
        byteq_ret = R_BYTEQ_Get(g_sock_tbl[socket_number].byteq_hdl, (data + recv_cnt));
        R_BSP_InterruptsEnable();
#endif
        if (BYTEQ_SUCCESS == byteq_ret)
        {
            recv_cnt++;
            continue;
        }

        /* timeout? */
        if (0 < timeout_ms)
        {
            tick_tmp = os_wrap_tickcount_get() - g_sock_tbl[socket_number].timer_rx.tick_sta;
            if (g_sock_tbl[socket_number].timer_rx.threshold <= tick_tmp)
            {
                WIFI_LOG_WARN(("R_WIFI_DA16XXX_ReceiveSocket: timeout!"));
                break;
            }
        }
        os_wrap_sleep(1, UNIT_TICK);
    }

    /* Not connect? */
    if ((recv_cnt == 0) && (DA16XXX_SOCKET_STATUS_CONNECTED != g_sock_tbl[socket_number].status))
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_ReceiveSocket: socket %d is not connected!", socket_number));
        return WIFI_ERR_SOCKET_NUM;
    }

    WIFI_LOG_INFO(("R_WIFI_DA16XXX_ReceiveSocket: socket %d recv_cnt=%d (%d).", socket_number, recv_cnt, tick_tmp));
    api_ret = recv_cnt;
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_ReceiveSocket
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_CloseSocket
 * Description  : Disconnect a specific socket connection.
 * Arguments    : socket_number
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_SOCKET_NUM
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_CloseSocket (uint32_t socket_number)
{
    wifi_err_t api_ret = WIFI_SUCCESS;
    e_rslt_code_t at_rep = AT_OK;
    uint8_t    cid = 0;

    /* Check parameters */
    if (socket_number >= SOCK_TBL_MAX)
    {
        return WIFI_ERR_SOCKET_NUM;
    }

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        return WIFI_SUCCESS;
    }

    /* get CID number */
    for (cid = g_sock_tbl[socket_number].type; cid < SOCK_TBL_MAX + 2; cid++)
    {
        if (g_sock_list[cid] == socket_number)
        {
            break;
        }
    }
    if (cid == SOCK_TBL_MAX + 2)
    {
        WIFI_LOG_WARN(("R_WIFI_DA16XXX_CloseSocket: socket is not opened!", socket_number));
        return WIFI_SUCCESS;
    }

    /* Connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        WIFI_LOG_WARN(("R_WIFI_DA16XXX_CloseSocket: Not connected to access point!"));
        goto CLOSE_SOCKET;
    }
    
    at_set_timeout(5000);
    at_rep = at_exec("AT+TRTRM=%d\r", cid);
    if (AT_INTERNAL_TIMEOUT != at_rep  && AT_OK != at_rep &&
        g_sock_tbl[socket_number].status != DA16XXX_SOCKET_STATUS_SOCKET)
    {
        api_ret = WIFI_ERR_MODULE_COM;
        goto RETURN_ERROR;
    }

CLOSE_SOCKET:
    R_BYTEQ_Flush(g_sock_tbl[socket_number].byteq_hdl);
    g_sock_tbl[socket_number].put_err_cnt = 0;
    g_sock_tbl[socket_number].status = DA16XXX_SOCKET_STATUS_CLOSED;
    g_sock_list[cid] = UINT8_MAX;
    WIFI_LOG_INFO(("R_WIFI_DA16XXX_CloseSocket: socket %d is closed!", socket_number));

RETURN_ERROR:
    at_set_timeout(ATCMD_RESP_TIMEOUT);
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_CloseSocket
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: R_WIFI_DA16XXX_HardwareReset
 * Description  : Reset the module.
 * Arguments    : none
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_SERIAL_OPEN
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_NOT_CONNECT
 *                WIFI_ERR_BYTEQ_OPEN
 *                WIFI_ERR_TAKE_MUTEX
 *                WIFI_ERR_SOCKET_CREATE
 *********************************************************************************************************************/
wifi_err_t R_WIFI_DA16XXX_HardwareReset (void)
{
    wifi_err_t api_ret = WIFI_SUCCESS;
    uint8_t cid = 0;
    uint8_t retry_count;
    uint8_t retry_max = 30;
    uint8_t sock_list_tmp[SOCK_TBL_MAX + 2];

    /* Disconnected WiFi module? */
    if (MODULE_DISCONNECTED == wifi_system_state_get())
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_HardwareReset: R_WIFI_DA16XXX_Open is not called!"));
        return WIFI_ERR_NOT_OPEN;
    }

    /* Take mutex */
    if (OS_WRAP_SUCCESS != os_wrap_mutex_take(MUTEX_TX))
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_HardwareReset: Failed to take mutex!"));
        return WIFI_ERR_TAKE_MUTEX;
    }

    /* Set WIFI State to "Connected WiFi module" */
    wifi_system_state_set(MODULE_CONNECTED);

    memset(sock_list_tmp, UINT8_MAX, sizeof(sock_list_tmp));
    for (cid = 0; cid < SOCK_TBL_MAX + 2; cid++)
    {
        if (g_sock_list[cid] != UINT8_MAX)
        {
            sock_list_tmp[cid] = g_sock_list[cid];
        }
    }

    /* Close UART BYTEQ */
    R_BYTEQ_Close(g_uart_tbl.byteq_hdl);

    /* Close UART port */
    uart_port_close();
    memset(&g_uart_tbl, 0, sizeof(g_uart_tbl));

    /* Reset WIFI module */
    da16xxx_hw_reset();

    /* Initialize BYTEQ on UART*/
    if (WIFI_SUCCESS != at_resp_byteq_open())
    {
        os_wrap_mutex_give(MUTEX_TX);
        return WIFI_ERR_BYTEQ_OPEN;
    }

    /* Open serial port */
    if (ATCMD_OK != uart_port_open(cb_sci_uart))
    {
        WIFI_LOG_ERROR(("R_WIFI_DA16XXX_HardwareReset: Failed to open SCI port!"));
        os_wrap_mutex_give(MUTEX_TX);
        return WIFI_ERR_SERIAL_OPEN;
    }

    /* Set flow control for serial port */
    flow_ctrl_init();

    /* Give mutex */
    os_wrap_mutex_give(MUTEX_TX);

    // check if R_WIFI_DA16XXX_Connect() is not called
    if (s_cur.ipaddress[0] == 0)
    {
        return api_ret;
    }

    WIFI_LOG_DEBUG(("Connecting to AccessPoint... \r\n"));
    for (retry_count = 0; retry_count < retry_max; retry_count++)
    {
        if (MODULE_ACCESSPOINT == wifi_system_state_get())
        {
            break;
        }
        else
        {
            os_wrap_sleep(1000, UNIT_MSEC);
        }
    }

    if (retry_count == retry_max)
    {
        WIFI_LOG_ERROR(("Cannot connect to AccessPoint. \r\n"));
        return WIFI_ERR_NOT_CONNECT;
    }

    /* Get IP address */
    R_WIFI_DA16XXX_GetIpAddress(&s_cur);
    WIFI_LOG_INFO(("Connected to AccessPoint\r\n"));

    /* reconnect socket */
    for (cid = 0; cid < SOCK_TBL_MAX + 2; cid++)
    {
        if (sock_list_tmp[cid] != UINT8_MAX)
        {
            WIFI_LOG_INFO(("Reconnecting to socket %d...\r\n", sock_list_tmp[cid]));
            api_ret = R_WIFI_DA16XXX_TcpConnect(sock_list_tmp[cid],
                                                g_sock_tbl[sock_list_tmp[cid]].ipaddr,
                                                g_sock_tbl[sock_list_tmp[cid]].port);
            if (api_ret != WIFI_SUCCESS)
            {
                break;
            }
        }
    }
    
    return api_ret;
}
/**********************************************************************************************************************
 * End of function R_WIFI_DA16XXX_HardwareReset
 *********************************************************************************************************************/

/*
 * System state control
 */
/**********************************************************************************************************************
 * Function Name: wifi_system_state_set
 * Description  : Set WIFI system state.
 * Arguments    : state
 * Return Value : none
 *********************************************************************************************************************/
static void wifi_system_state_set(e_wifi_module_status_t state)
{
    s_wifi_system_state = state;
}
/**********************************************************************************************************************
 * End of function wifi_system_state_set
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: wifi_system_state_get
 * Description  : Get WiFi system state.
 * Arguments    : none
 * Return Value : wifi_system_status_t WIFI system state
 *********************************************************************************************************************/
static e_wifi_module_status_t wifi_system_state_get(void)
{
    return s_wifi_system_state;
}
/**********************************************************************************************************************
 * End of function wifi_system_state_get
 *********************************************************************************************************************/

/*
 * Sub functions for API
 */
/**********************************************************************************************************************
 * Function Name: disconnect_ap_sub
 * Description  : Disconnect access point (sub function).
 * Arguments    : none
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_MODULE_COM
 *********************************************************************************************************************/
static wifi_err_t disconnect_ap_sub(void)
{
    /* Not connected access point? */
    if (0 != R_WIFI_DA16XXX_IsConnected())
    {
        return WIFI_SUCCESS;
    }

    /* Disconnect from currently connected Access Point */
    if (AT_OK != at_exec("AT+WFQAP\r"))
    {
        return WIFI_ERR_MODULE_COM;
    }

    return WIFI_SUCCESS;
}
/**********************************************************************************************************************
 * End of function disconnect_ap_sub
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: da16xxx_handle_incoming_socket_data
 * Description  : Handles incoming socket data.
 * Arguments    : none
 * Return Value : none
 *********************************************************************************************************************/
static void da16xxx_handle_incoming_socket_data(uint8_t data)
{
    uint8_t cid = UINT8_MAX;

    switch (g_uart_tbl.socket_recv_state)
    {
        case DA16XXX_RECV_PREFIX:
        {
            if ('+' == data)
            {
                g_uart_tbl.socket_recv_state = DA16XXX_RECV_CMD;
                g_rx_idx = 0;
            }
            break;
        }
        case DA16XXX_RECV_CMD:
        {
            g_rx_buff[g_rx_idx++] = data;
            g_rx_idx = g_rx_idx % TEMP_BUf_MAX;

            if (5 == g_rx_idx)
            {
                /* Check for incoming data through socket */
                if (0 == strncmp("TRDTC", (char *) g_rx_buff, g_rx_idx))
                {
                    g_uart_tbl.at_resp_type = DA16XXX_RESP_TRDTC;
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_SUFFIX;
                }
                /* Check for TCP socket disconnect notification */
                else if (0 == strncmp("TRXTC", (char *) g_rx_buff, g_rx_idx))
                {
                    g_uart_tbl.at_resp_type = DA16XXX_RESP_TRXTC;
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_SUFFIX;
                }
                /* Check AP connect status */
                else if (0 == strncmp("WFJAP", (char *) g_rx_buff, g_rx_idx))
                {
                    // check if R_WIFI_DA16XXX_Connect() is not called
                    if (s_cur.ipaddress[0] == 0)
                    {
                        g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
                    }
                    else
                    {
                        g_uart_tbl.at_resp_type = DA16XXX_RESP_WFJAP;
                        g_uart_tbl.socket_recv_state = DA16XXX_RECV_SUFFIX;
                    }
                }
                /* Check AP disconnect status */
                else if (0 == strncmp("WFDAP", (char *) g_rx_buff, g_rx_idx))
                {
                    // check if R_WIFI_DA16XXX_Connect() is called
                    if (s_cur.ipaddress[0] != 0)
                    {
                        s_wifi_system_state = MODULE_CONNECTED;
                    }
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
                }
                else
                {
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
                }
            }
            else if ('+' == data)
            {
                // reset index
                g_rx_idx = 0;
            }
            break;
        }
        case DA16XXX_RECV_SUFFIX:
        {
            if (':' == data)
            {
                g_uart_tbl.socket_recv_state = DA16XXX_RECV_PARAM_CID;
                g_rx_idx = 0;
            }
            break;
        }
        case DA16XXX_RECV_PARAM_CID:
        {
            if (',' == data)
            {
                g_rx_buff[g_rx_idx] = 0;
                cid = strtol((char *) g_rx_buff, NULL, 10);
                if (cid == 1 && g_uart_tbl.at_resp_type == DA16XXX_RESP_WFJAP)
                {
                    s_wifi_system_state = MODULE_ACCESSPOINT;
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
                }
                else if (g_uart_tbl.at_resp_type == DA16XXX_RESP_TRXTC)
                {
                    /* Socket disconnect event received */
                    g_cur_sock_idx = g_sock_list[cid];
                    g_sock_tbl[g_cur_sock_idx].put_err_cnt = 0;
                    g_sock_tbl[g_cur_sock_idx].status = DA16XXX_SOCKET_STATUS_SOCKET;
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
                }
                else if (g_uart_tbl.at_resp_type == DA16XXX_RESP_TRDTC)
                {
                    g_cur_sock_idx = g_sock_list[cid];
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_PARAM_IP;
                }
                else
                {
                    g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
                }
            }
            else
            {
                g_rx_buff[g_rx_idx++] = data;
                g_rx_idx = g_rx_idx % TEMP_BUf_MAX;
            }
            break;
        }
        case DA16XXX_RECV_PARAM_IP:
        {
            if (',' == data)
            {
                g_uart_tbl.socket_recv_state = DA16XXX_RECV_PARAM_PORT;
            }
            break;
        }
        case DA16XXX_RECV_PARAM_PORT:
        {
            if (',' == data)
            {
                g_uart_tbl.socket_recv_state = DA16XXX_RECV_PARAM_LEN;
                g_rx_idx = 0;
            }
            break;
        }
        case DA16XXX_RECV_PARAM_LEN:
        {
            if (',' == data)
            {
                g_rx_buff[g_rx_idx] = 0;
                g_uart_tbl.socket_recv_state = DA16XXX_RECV_DATA;
                g_sock_tbl[g_cur_sock_idx].recv_len = strtol((char *) g_rx_buff, NULL, 10);
            }
            else
            {
                g_rx_buff[g_rx_idx++] = data;
                g_rx_idx = g_rx_idx % TEMP_BUf_MAX;
            }
            break;
        }

        case DA16XXX_RECV_DATA:
        {
            if (0 < g_sock_tbl[g_cur_sock_idx].recv_len--)
            { 
                if (BYTEQ_SUCCESS != R_BYTEQ_Put(g_sock_tbl[g_cur_sock_idx].byteq_hdl, data))
                {
                    g_sock_tbl[g_cur_sock_idx].put_err_cnt++;
                    post_err_event(WIFI_EVENT_SOCKET_RXQ_OVF_ERR, g_cur_sock_idx);
                }
            }
            break;
        }
    }
    if (g_uart_tbl.socket_recv_state != DA16XXX_RECV_DATA)
    {
        if (BYTEQ_SUCCESS != R_BYTEQ_Put(g_uart_tbl.byteq_hdl, data))
        {
            if (g_cur_sock_idx != UINT8_MAX)
            {
                g_sock_tbl[g_cur_sock_idx].put_err_cnt++;
            }
            post_err_event(WIFI_EVENT_SOCKET_RXQ_OVF_ERR, g_cur_sock_idx);
        }
    }
    else if (0 >= g_sock_tbl[g_cur_sock_idx].recv_len)
    {
        g_cur_sock_idx = UINT8_MAX;
        g_uart_tbl.socket_recv_state = DA16XXX_RECV_PREFIX;
    }
}
/**********************************************************************************************************************
 * End of function da16xxx_handle_incoming_socket_data
 *********************************************************************************************************************/

/*
 * BYTEQ control for socket
 */
/**********************************************************************************************************************
 * Function Name: socket_byteq_open
 * Description  : open BYTEQ in socket table.
 * Arguments    : max_sock - maximum of socket tables
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_BYTEQ_OPEN
 *********************************************************************************************************************/
static wifi_err_t socket_byteq_open(void)
{
    uint8_t i;
    wifi_err_t api_ret = WIFI_SUCCESS;

    for (i = 0; i < SOCK_TBL_MAX; i++ )
    {
        if (BYTEQ_SUCCESS !=
                R_BYTEQ_Open(g_sock_tbl[i].recv_buf, SOCK_BUF_MAX, &g_sock_tbl[i].byteq_hdl))
        {
            WIFI_LOG_ERROR(("socket_byteq_open: Cannot open BYTEQ for socket %d!", i));
            api_ret = WIFI_ERR_BYTEQ_OPEN;
            break;
        }
    }
    return api_ret;
}
/**********************************************************************************************************************
 * End of function socket_byteq_open
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: socket_byteq_close
 * Description  : close BYTEQ on socket table.
 * Arguments    : none
 * Return Value : none
 *********************************************************************************************************************/
static void socket_byteq_close(void)
{
    uint8_t i;

    for (i = 0; i < SOCK_TBL_MAX; i++ )
    {
        if (0 != g_sock_tbl[i].byteq_hdl)
        {
            R_BYTEQ_Close(g_sock_tbl[i].byteq_hdl);
        }
    }
}
/**********************************************************************************************************************
 * End of function socket_byteq_close
 *********************************************************************************************************************/

/*
 * WIFI module control
 */
/**********************************************************************************************************************
 * Function Name: da16xxx_hw_reset
 * Description  : Reset DA16XXX.
 * Arguments    : none
 * Return Value : none
 *********************************************************************************************************************/
static void da16xxx_hw_reset(void)
{
    /* Phase 3 WIFI Module hardware reset   */
    WIFI_RESET_DDR(WIFI_CFG_RESET_PORT, WIFI_CFG_RESET_PIN) = 1;  /* output */
    WIFI_RESET_DR(WIFI_CFG_RESET_PORT, WIFI_CFG_RESET_PIN)  = 0;  /* low    */
#if (WIFI_CFG_DA16600_SUPPORT)
    R_BSP_SoftwareDelay(500, BSP_DELAY_MILLISECS);
#else
    R_BSP_SoftwareDelay(30, BSP_DELAY_MILLISECS);
#endif
    WIFI_RESET_DR(WIFI_CFG_RESET_PORT, WIFI_CFG_RESET_PIN)  = 1;  /* high   */
    R_BSP_SoftwareDelay(250, BSP_DELAY_MILLISECS);
}
/**********************************************************************************************************************
 * End of function da16xxx_hw_reset
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: da16xxx_close
 * Description  : Close WIFI module.
 * Arguments    : none
 * Return Value : none
 *********************************************************************************************************************/
static void da16xxx_close(void)
{
    R_BYTEQ_Close(g_uart_tbl.byteq_hdl);
    uart_port_close();
    os_wrap_mutex_delete();
    socket_byteq_close();
    wifi_system_state_set(MODULE_DISCONNECTED);
}
/**********************************************************************************************************************
 * End of function da16xxx_close
 *********************************************************************************************************************/

#if WIFI_CFG_SNTP_ENABLE == 1
/**********************************************************************************************************************
 * Function Name: da16xxx_sntp_service_init
 * Description  : Initialize SNTP client service.
 * Arguments    : none
 * Return Value : WIFI_SUCCESS
 *                WIFI_ERR_PARAMETER
 *                WIFI_ERR_MODULE_COM
 *                WIFI_ERR_TAKE_MUTEX
 *********************************************************************************************************************/
static wifi_err_t da16xxx_sntp_service_init(void)
{
    wifi_err_t api_ret = WIFI_SUCCESS;
    int ip_address_sntp_server[4];

    /* Set the SNTP server IP address */
    if (4 != sscanf(WIFI_CFG_SNTP_SERVER_IP, "%d.%d.%d.%d",
                    &ip_address_sntp_server[0], &ip_address_sntp_server[1],
                    &ip_address_sntp_server[2], &ip_address_sntp_server[3]))
    {
        WIFI_LOG_ERROR(("da16xxx_sntp_service_init: Wrong format of SNTP server IP address!"));
        return WIFI_ERR_PARAMETER;
    }

    /* Configure the SNTP Server Address */
    api_ret = R_WIFI_DA16XXX_SntpServerIpAddressSet((uint8_t *) ip_address_sntp_server);
    if (api_ret != WIFI_SUCCESS)
    {
        return api_ret;
    }

    /* Enable the SNTP client */
    api_ret = R_WIFI_DA16XXX_SntpEnableSet((wifi_sntp_enable_t) WIFI_CFG_SNTP_ENABLE);
    if (api_ret != WIFI_SUCCESS)
    {
        return api_ret;
    }

    /* Set the SNTP timezone configuration string */
    api_ret = R_WIFI_DA16XXX_SntpTimeZoneSet(WIFI_CFG_SNTP_UTC_OFFSET);
    if (api_ret != WIFI_SUCCESS)
    {
        return api_ret;
    }

    return api_ret;
}
/**********************************************************************************************************************
 * End of function da16xxx_sntp_service_init
 *********************************************************************************************************************/
 #endif

/*
 * Callback functions
 */
/**********************************************************************************************************************
 * Function Name: cb_sci_uart
 * Description  : SCI callback function of UART.
 * Arguments    : pArgs
 * Return Value : none
 *********************************************************************************************************************/
static void cb_sci_uart(void * pArgs)
{
    uint8_t data;
    sci_cb_args_t * p_args = (sci_cb_args_t *) pArgs;

    if (SCI_EVT_RX_CHAR == p_args->event)
    {
        R_SCI_Receive(g_uart_tbl.sci_hdl, &data, 1);
        da16xxx_handle_incoming_socket_data(data);
    }
    else if (SCI_EVT_TEI == p_args->event)
    {
        g_uart_tbl.tx_end_flag = 1;
    }
    else
    {
        cb_sci_err(p_args->event);
    }
}
/**********************************************************************************************************************
 * End of function cb_sci_uart
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: cb_sci_err
 * Description  : SCI callback function of error event.
 * Arguments    : event
 * Return Value : none
 *********************************************************************************************************************/
static void cb_sci_err(sci_cb_evt_t event)
{
    if (SCI_EVT_RXBUF_OVFL == event)
    {
        /* From RXI interrupt; rx queue is full */
        // WIFI_LOG_ERROR(("cb_sci_err: SCI_EVT_RXBUF_OVFL"));
        post_err_event(WIFI_EVENT_SERIAL_RXQ_OVF_ERR, 0);
    }
    else if (SCI_EVT_OVFL_ERR == event)
    {
        /* From receiver overflow error interrupt */
        // WIFI_LOG_ERROR(("cb_sci_err: SCI_EVT_OVFL_ERR"));
        post_err_event(WIFI_EVENT_SERIAL_OVF_ERR, 0);
    }
    else if (SCI_EVT_FRAMING_ERR == event)
    {
        /* From receiver framing error interrupt */
        // WIFI_LOG_ERROR(("cb_sci_err: SCI_EVT_FRAMING_ERR"));
        post_err_event(WIFI_EVENT_SERIAL_FLM_ERR, 0);
    }
    else
    {
        /* Do nothing */
        WIFI_LOG_INFO(("cb_sci_err: event %d.", event));
    }
}
/**********************************************************************************************************************
 * End of function cb_sci_err
 *********************************************************************************************************************/

/*
 * Log functions
 */
/**********************************************************************************************************************
 * Function Name: s_vsnprintf_safe
 * Description  : vsnprintf.
 * Arguments    : s - string buffer
 * 		          n - length
 *                format - format message
 *                varg
 * Return Value : None
 *********************************************************************************************************************/
static int s_vsnprintf_safe(char * s,
                            size_t n,
                            const char * format,
                            va_list arg)
{
    int ret;

    ret = vsnprintf(s, n, format, arg);

    /* Check if the string was truncated and if so, update the return value
     * to reflect the number of characters actually written. */
    if( ret >= n )
    {
        /* Do not include the terminating NULL character to keep the behaviour
         * same as the standard. */
        ret = n - 1;
    }
    else if( ret < 0 )
    {
        /* Encoding error - Return 0 to indicate that nothing was written to the
         * buffer. */
        ret = 0;
    }
    else
    {
        /* Complete string was written to the buffer. */
    }

    return ret;
}
/**********************************************************************************************************************
 * End of function s_vsnprintf_safe
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: s_snprintf_safe
 * Description  : snprintf.
 * Arguments    : s - string buffer
 * 		          n - length
 *                format - format message
 * Return Value : None
 *********************************************************************************************************************/
static int s_snprintf_safe(char * s,
                          size_t n,
                          const char * format,
                          ... )
{
    int ret;
    va_list args;

    va_start( args, format );
    ret = s_vsnprintf_safe( s, n, format, args );
    va_end( args );

    return ret;
}
/**********************************************************************************************************************
 * End of function s_snprintf_safe
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: s_log_printf_common
 * Description  : Print common log without freeRTOS logging module.
 * Arguments    : usLoggingLevel - log level
 *                format - format message
 *                varg
 * Return Value : None
 *********************************************************************************************************************/
static void s_log_printf_common(uint8_t usLoggingLevel,
                                const char * pcFormat,
                                va_list args)
{
    size_t xLength = 0;
    char pcPrintString[256];

    if (NULL != pcPrintString)
    {
        const int8_t * pcLevelString = NULL;
        size_t ulFormatLen = 0UL;

        /* Choose the string for the log level metadata for the log message. */
        switch (usLoggingLevel)
        {
            case 1:
                pcLevelString = "ERROR";
                break;

            case 2:
                pcLevelString = "WARN";
                break;

            case 3:
                pcLevelString = "INFO";
                break;

            case 4:
                pcLevelString = "DEBUG";
                break;

            default:
                pcLevelString = NULL;
                break;
        }

        /* Add the chosen log level information as prefix for the message. */
        if( ( pcLevelString != NULL ) && ( xLength < 256 ) )
        {
            xLength += s_snprintf_safe( pcPrintString + xLength, 256 - xLength, "[%s] ", pcLevelString );
        }

        if( xLength < 256 )
        {
            xLength += s_vsnprintf_safe( pcPrintString + xLength, 256 - xLength, pcFormat, args );
        }

        /* Add newline characters if the message does not end with them.*/
        ulFormatLen = strlen( (char *)pcFormat );

        if( ( ulFormatLen >= 2 ) &&
            ( strncmp( (char *)(pcFormat + ulFormatLen), "\r\n", 2 ) != 0 ) &&
            ( xLength < 256 ) )
        {
            xLength += s_snprintf_safe(pcPrintString + xLength, 256 - xLength, "%s", "\r\n" );
        }

        pcPrintString[xLength] = '\0';

        /* Only send the buffer to the logging task if it is
         * not empty. */
        if (xLength > 0)
        {
            printf ("%s", pcPrintString);
        }
    }
}
/**********************************************************************************************************************
 * End of function s_log_printf_common
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: printf_log_error
 * Description  : Print error log without freeRTOS logging module.
 * Arguments    : format - format message
 * Return Value : None
 *********************************************************************************************************************/
void printf_log_error (const char * format, ...)
{
    va_list args;

    va_start(args, format);
    s_log_printf_common(1, format, args);

    va_end(args);
}
/**********************************************************************************************************************
 * End of function printf_log_error
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: printf_log_warn
 * Description  : Print warning log without freeRTOS logging module.
 * Arguments    : format - format message
 * Return Value : None
 *********************************************************************************************************************/
void printf_log_warn (const char * format, ...)
{
    va_list args;

    va_start(args, format);
    s_log_printf_common(2, format, args);

    va_end(args);
}
/**********************************************************************************************************************
 * End of function printf_log_warn
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: printf_log_info
 * Description  : Print information log without freeRTOS logging module.
 * Arguments    : format - format message
 * Return Value : None
 *********************************************************************************************************************/
void printf_log_info (const char * format, ...)
{
    va_list args;

    va_start(args, format);
    s_log_printf_common(3, format, args);

    va_end(args);
}
/**********************************************************************************************************************
 * End of function printf_log_info
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Function Name: printf_log_debug
 * Description  : Print debug (AT command) log without freeRTOS logging module.
 * Arguments    : format - format message
 * Return Value : None
 *********************************************************************************************************************/
void printf_log_debug (const char * format, ...)
{
    va_list args;

    va_start(args, format);
    s_log_printf_common(4, format, args);

    va_end(args);
}
/**********************************************************************************************************************
 * End of function printf_log_debug
 *********************************************************************************************************************/