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
 * Copyright (C) 2022 Renesas Electronics Corporation. All rights reserved.
 *********************************************************************************************************************/
/**********************************************************************************************************************
 * File Name    : r_cellular_setpsm.c
 * Description  : Function to set the PSM of the module.
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Includes   <System Includes> , "Project Includes"
 *********************************************************************************************************************/
#include "cellular_private_api.h"
#include "cellular_freertos.h"
#include "at_command.h"

/**********************************************************************************************************************
 * Macro definitions
 *********************************************************************************************************************/
#define PHASE_1    (0x01 << 0)
#define PHASE_2    (0x01 << 1)
#define PHASE_3    (0x01 << 2)
#define PHASE_4    (0x01 << 3)
#define PHASE_5    (0x01 << 4)
#define PHASE_6    (0x01 << 5)

/**********************************************************************************************************************
 * Typedef definitions
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Exported global variables
 *********************************************************************************************************************/

/**********************************************************************************************************************
 * Private (static) variables and functions
 *********************************************************************************************************************/
static e_cellular_err_t cellular_psm_config (st_cellular_ctrl_t * const p_ctrl, const e_cellular_psm_mode_t mode);
static void cellular_psm_config_fail (st_cellular_ctrl_t * const p_ctrl, const uint8_t open_phase);

/************************************************************************
 * Function Name  @fn            R_CELLULAR_SetPSM
 ***********************************************************************/
e_cellular_err_t R_CELLULAR_SetPSM(st_cellular_ctrl_t * const p_ctrl, const st_cellular_psm_config_t * const p_config,
                                    st_cellular_psm_config_t * const p_result)
{
    e_cellular_err_t ret = CELLULAR_SUCCESS;
    e_cellular_err_semaphore_t semaphore_ret = CELLULAR_SEMAPHORE_SUCCESS;

    if ((NULL == p_ctrl) || ((CELLULAR_PSM_MODE_INVALID > p_config->psm_mode) || (CELLULAR_PSM_MODE_INIT < p_config->psm_mode)) ||
            ((CELLULAR_TAU_CYCLE_10_MIN > p_config->tau_cycle) || (CELLULAR_TAU_CYCLE_NONE < p_config->tau_cycle)) ||
            ((CELLULAR_ACTIVE_CYCLE_2_SEC > p_config->active_cycle) || (CELLULAR_ACTIVE_CYCLE_NONE < p_config->active_cycle)) ||
            ((CELLULAR_CYCLE_MULTIPLIER_0 > p_config->tau_multiplier) || (CELLULAR_CYCLE_MULTIPLIER_31 < p_config->tau_multiplier)) ||
            ((CELLULAR_CYCLE_MULTIPLIER_0 > p_config->active_multiplier) || (CELLULAR_CYCLE_MULTIPLIER_31 < p_config->active_multiplier)))
    {
        ret = CELLULAR_ERR_PARAMETER;
    }
    else
    {
        if (CELLULAR_SYSTEM_CLOSE == p_ctrl->system_state)
        {
            ret = CELLULAR_ERR_NOT_OPEN;
        }
    }

    if (CELLULAR_SUCCESS == ret)
    {
        semaphore_ret = cellular_take_semaphore(p_ctrl->at_semaphore);
        if (CELLULAR_SEMAPHORE_SUCCESS == semaphore_ret)
        {
            ret = cellular_psm_config(p_ctrl, p_config->psm_mode);
            if (CELLULAR_SUCCESS == ret)
            {
                ret = atc_cpsms(p_ctrl, p_config);
            }
            if ((CELLULAR_SUCCESS == ret) && (NULL != p_result))
            {
                p_ctrl->recv_data = p_result;
                ret = atc_cpsms_check(p_ctrl);
                cellular_delay_task(1000);
            }
            cellular_give_semaphore(p_ctrl->at_semaphore);

            if (CELLULAR_SUCCESS == ret)
            {
                ret = cellular_module_reset(p_ctrl);
            }
        }
        else
        {
            ret = CELLULAR_ERR_OTHER_ATCOMMAND_RUNNING;
        }
    }

    return ret;
}
/**********************************************************************************************************************
 * End of function R_CELLULAR_SetPSM
 *********************************************************************************************************************/

/************************************************************************
 * Function Name  @fn            cellular_psm_config
 ***********************************************************************/
static e_cellular_err_t cellular_psm_config(st_cellular_ctrl_t * const p_ctrl, const e_cellular_psm_mode_t mode)
{
    e_cellular_err_t ret = CELLULAR_SUCCESS;
    uint8_t open_phase = 0;

    if (CELLULAR_PSM_MODE_ACTIVE == mode)
    {
        p_ctrl->ring_ctrl.ring_event = cellular_create_event_group("ring_event");

        if (NULL != p_ctrl->ring_ctrl.ring_event)
        {
            open_phase |= PHASE_1;
            p_ctrl->ring_ctrl.rts_semaphore = cellular_create_semaphore("rts_semaphore");
        }

        if (NULL != p_ctrl->ring_ctrl.rts_semaphore)
        {
            open_phase |= PHASE_2;
            ret = cellular_start_ring_task(p_ctrl);
        }

        if (CELLULAR_SUCCESS == ret)
        {
            open_phase |= PHASE_3;
            ret = cellular_irq_open(p_ctrl);
        }

        if (CELLULAR_SUCCESS == ret)
        {
            open_phase |= PHASE_4;
            ret = atc_sqnricfg(p_ctrl, CELLULAR_SQNRICFG_MODE);
        }

        if (CELLULAR_SUCCESS == ret)
        {
            open_phase |= PHASE_5;
            ret = atc_sqnipscfg(p_ctrl, CELLULAR_SQNIPSCFG_MODE);
        }

        if (CELLULAR_SUCCESS == ret)
        {
            open_phase |= PHASE_6;
            ret = atc_sqnpscfg(p_ctrl);
        }

        if (CELLULAR_SUCCESS == ret)
        {
            p_ctrl->ring_ctrl.psm = CELLULAR_PSM_ACTIVE;
        }
        else
        {
            cellular_psm_config_fail(p_ctrl, open_phase);
        }
    }
    else
    {
        p_ctrl->ring_ctrl.psm = CELLULAR_PSM_DEACTIVE;

        ret = atc_sqnricfg(p_ctrl, CELLULAR_PSM_MODE_INVALID);

        if (CELLULAR_SUCCESS == ret)
        {
            ret = atc_sqnipscfg(p_ctrl, CELLULAR_PSM_MODE_INVALID);
        }

        cellular_irq_close(p_ctrl);

        if (NULL != p_ctrl->ring_ctrl.ring_taskhandle)
        {
            cellular_delete_task(p_ctrl->ring_ctrl.ring_taskhandle);
            p_ctrl->ring_ctrl.ring_taskhandle = NULL;
        }
        if (NULL != p_ctrl->ring_ctrl.rts_semaphore)
        {
            cellular_delete_semaphore(p_ctrl->ring_ctrl.rts_semaphore);
            p_ctrl->ring_ctrl.rts_semaphore = NULL;
        }
        if (NULL != p_ctrl->ring_ctrl.ring_event)
        {
            cellular_delete_event_group(p_ctrl->ring_ctrl.ring_event);
            p_ctrl->ring_ctrl.ring_event = NULL;
        }

        cellular_rts_ctrl(0);

    }

    return ret;
}
/**********************************************************************************************************************
 * End of function cellular_psm_config
 *********************************************************************************************************************/

/************************************************************************
 * Function Name  @fn            cellular_psm_config_fail
 ***********************************************************************/
static void cellular_psm_config_fail(st_cellular_ctrl_t * const p_ctrl, const uint8_t open_phase)
{
    if ((open_phase & PHASE_6) == PHASE_6)
    {
        atc_sqnricfg(p_ctrl, CELLULAR_PSM_MODE_INVALID);
    }

    if ((open_phase & PHASE_5) == PHASE_5)
    {
        atc_sqnipscfg(p_ctrl, CELLULAR_PSM_MODE_INVALID);
    }

    if ((open_phase & PHASE_4) == PHASE_4)
    {
        cellular_irq_close(p_ctrl);
    }

    if ((open_phase & PHASE_3) == PHASE_3)
    {
        cellular_delete_task(p_ctrl->ring_ctrl.ring_taskhandle);
        p_ctrl->ring_ctrl.ring_taskhandle = NULL;
    }

    if ((open_phase & PHASE_2) == PHASE_2)
    {
        cellular_delete_semaphore(p_ctrl->ring_ctrl.rts_semaphore);
        p_ctrl->ring_ctrl.rts_semaphore = NULL;
    }

    if ((open_phase & PHASE_1) == PHASE_1)
    {
        cellular_delete_event_group(p_ctrl->ring_ctrl.ring_event);
        p_ctrl->ring_ctrl.ring_event = NULL;
    }

    return;
}
/**********************************************************************************************************************
 * End of function cellular_psm_config_fail
 *********************************************************************************************************************/