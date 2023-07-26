/*
 * FreeRTOS V202012.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 * Modifications Copyright (C) 2023 Renesas Electronics Corporation. or its affiliates.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file ota_demo_config.h
 * @brief Configuration options for the OTA related demos.
 */

#ifndef OTA_DEMO_CONFIG_H_
#define OTA_DEMO_CONFIG_H_
#include "test_param_config.h"

/**
 * @brief Certificate used for validating code signing signatures in the OTA PAL.
 */
#include "test_execution_config.h"
#if ( OTA_PAL_TEST_ENABLED )
    #include "aws_test_ota_pal_ecdsa_sha256_signature.h"
	#define otapalconfigCODE_SIGNING_CERTIFICATE OTA_PAL_CODE_SIGNING_CERTIFICATE
#endif

/**
 * @brief Major version of the firmware.
 *
 * This is used in the OTA demo to set the appFirmwareVersion variable that is
 * declared in the ota_appversion32.h file in the OTA library.
 */
#ifndef APP_VERSION_MAJOR
    #define APP_VERSION_MAJOR    OTA_APP_VERSION_MAJOR
#endif

/**
 * @brief Minor version of the firmware.
 *
 * This is used in the OTA demo to set the appFirmwareVersion variable that is
 * declared in the ota_appversion32.h file in the OTA library.
 */
#ifndef APP_VERSION_MINOR
    #define APP_VERSION_MINOR    OTA_APP_VERSION_MINOR
#endif

/**
 * @brief Build version of the firmware.
 *
 * This is used in the OTA demo to set the appFirmwareVersion variable that is
 * declared in the ota_appversion32.h file in the OTA library.
 */
#ifndef APP_VERSION_BUILD
    #define APP_VERSION_BUILD    OTA_APP_VERSION_BUILD
#endif

/**
 * @brief Server's root CA certificate.
 *
 * This certificate is used to identify the AWS IoT server and is publicly
 * available. Refer to the AWS documentation available in the link below for
 * information about the Server Root CAs.
 * https://docs.aws.amazon.com/iot/latest/developerguide/server-authentication.html#server-authentication-certs
 *
 * @note The TI C3220 Launchpad board requires that the Root CA have its
 * certificate self-signed. As mentioned in the above link, the Amazon Root CAs
 * are cross-signed by the Starfield Root CA. Thus, ONLY the Starfield Root CA
 * can be used to connect to the ATS endpoints on AWS IoT for the TI board.
 *
 * @note This certificate should be PEM-encoded.
 *
 * Must include the PEM header and footer:
 * "-----BEGIN CERTIFICATE-----\n"\
 * "...base64 data...\n"\
 * "-----END CERTIFICATE-----\n"
 *
 */
#define democonfigROOT_CA_PEM                   tlsSTARFIELD_ROOT_CERTIFICATE_PEM

/**
 * @brief The length of the queue used to hold commands for the agent.
 */
#define MQTT_AGENT_COMMAND_QUEUE_LENGTH         ( 25 )

/**
 * @brief Dimensions the buffer used to serialise and deserialise MQTT packets.
 * @note Specified in bytes.  Must be large enough to hold the maximum
 * anticipated MQTT payload.
 */
#define MQTT_AGENT_NETWORK_BUFFER_SIZE          ( 5000 )

/**
 * @brief Maximum time MQTT agent waits in the queue for any pending MQTT
 * operations.
 *
 * The wait time is kept smallest possible to increase the responsiveness of
 * MQTT agent while processing  pending MQTT operations as well as receive
 * packets from network.
 */
#define MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME    ( 50U )

#define MQTT_COMMAND_CONTEXTS_POOL_SIZE              ( 10 )

#endif /* OTA_DEMO_CONFIG_H_ */
