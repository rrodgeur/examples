/*
 * Copyright (c) 2021-2024 LAAS-CNRS
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGLPV2.1
 */

/**
 * @brief  This file it the main entry point of the
 *         OwnTech Power API. Please check the OwnTech
 *         documentation for detailed information on
 *         how to use Power API: https://docs.owntech.org/
 *
 * @author Clément Foucher <clement.foucher@laas.fr>
 * @author Luiz Villa <luiz.villa@laas.fr>
 * @author Ayoub Farah Hassan <ayoub.farah-hassan@laas.fr>
 * @author Régis Ruelland <regis.ruelland@laas.fr>
 */

//--------------OWNTECH APIs----------------------------------
#include "DataAPI.h"
#include "TaskAPI.h"
#include "TwistAPI.h"
#include "CommunicationAPI.h"

#include "zephyr/console/console.h"

#include "trigo.h"
#include "pr.h"

#define CLIENT      // Role : SERVER or CLIENT 

#ifdef SERVER
#define STR_ROLE "SERVER"
#else
#define STR_ROLE "CLIENT"
#endif
//--------------SETUP FUNCTIONS DECLARATION-------------------
void setup_routine(); // Setups the hardware and software of the system

//--------------LOOP FUNCTIONS DECLARATION--------------------
void loop_communication_task(); // code to be executed in the slow communication task
void loop_application_task();   // Code to be executed in the background task
void loop_critical_task();     // Code to be executed in real time in the critical task

//--------------USER VARIABLES DECLARATIONS-------------------
static uint32_t control_task_period = 100; //[us] period of the control task
static bool pwm_enable = false;            //[bool] state of the PWM (ctrl task)

uint8_t received_serial_char;

/* Measure variables */
static float32_t V1_low_value;
static float32_t V2_low_value;
static float32_t I1_low_value;
static float32_t I2_low_value;
static float32_t V_high;

static float32_t Iref; // current reference from SERVER
static float32_t Vgrid; // voltage reference from SERVER

static float meas_data; // temp storage meas value (ctrl task)

/* duty_cycle*/
static float32_t duty_cycle;

/* Sinewave settings */
static const float Udc = 40.0;
static const float f0 = 50;

//------------- PR RESONANT -------------------------------------
static float32_t w0 = 2 * PI *f0;
static float32_t Ts = control_task_period * 1.0e-6f;
#ifdef SERVER
static float angle = 0;
static float32_t Vgrid_amplitude = 12.0; 
static float32_t k_gain = 1.0;

static Pr pr_voltage;
static float32_t Kp_v = 0.02F;
static float32_t Kr_v = 4000.0F;
static PrParams pr_voltage_params(Ts, Kp_v, Kr_v, w0, 0.0, -Udc, Udc);
#else 
static float32_t Kp_i = 0.2;
static float32_t Kr_i = 3000.0;
static Pr pr_current;
static PrParams pr_current_params(Ts, Kp_i, Kr_i, w0, 0.0, -Udc, Udc);
static float32_t pr_value;
#endif

struct consigne_struct
{
    float32_t Vref_fromSERVER;
    float32_t Iref_fromSERVER;
    float32_t w0_fromSERVER;
    uint8_t id_and_status; // Contains status
};

struct consigne_struct tx_consigne;
struct consigne_struct rx_consigne;
uint8_t* buffer_tx = (uint8_t*)&tx_consigne;
uint8_t* buffer_rx =(uint8_t*)&rx_consigne;

uint8_t status;
uint32_t counter_time;

typedef struct Record
{
    float32_t I_low;
    float32_t V_low;
    float32_t Vhigh_value;
    float32_t Iref;
    float32_t duty_cycle;
    float32_t Vgrid;
    float32_t angle;
} record_t;

bool is_downloading;
const uint16_t RECORD_SIZE = 2048;
const uint16_t NB_CURVES = 7;
record_t record_array[RECORD_SIZE];
uint32_t counter;

//---------------------------------------------------------------

enum serial_interface_menu_mode // LIST OF POSSIBLE MODES FOR THE OWNTECH CONVERTER
{
    IDLEMODE = 0,
    POWERMODE
};

uint8_t mode = IDLEMODE;

//--------------SETUP FUNCTIONS-------------------------------

void reception_function(void)
{
#ifdef CLIENT
    if (rx_consigne.id_and_status >> 6 == 1)
    {
        status = rx_consigne.id_and_status;

        if ((rx_consigne.id_and_status & 2) == 1)
            counter = 0;

        Iref = rx_consigne.Iref_fromSERVER;
        Vgrid = rx_consigne.Vref_fromSERVER;
        w0 = rx_consigne.w0_fromSERVER;
    }
#endif
}

/**
 * This is the setup routine.
 * It is used to call functions that will initialize your spin, twist, data and/or tasks.
 * In this example, we setup the version of the spin board and a background task.
 * The critical task is defined but not started.
 */
void setup_routine()
{
    // Setup the hardware first
    spin.version.setBoardVersion(SPIN_v_1_0);
    twist.setVersion(shield_TWIST_V1_3);

    data.enableTwistDefaultChannels();

    /* buck voltage mode */
    twist.initLegBuck(LEG1);
    twist.initLegBoost(LEG2);

    communication.rs485.configure(buffer_tx, buffer_rx, sizeof(consigne_struct), reception_function, SPEED_20M); // custom configuration for RS485

#ifdef SERVER
    communication.sync.initMaster(); // start the synchronisation
    pr_voltage.init(pr_voltage_params);
#endif

#ifndef SERVER
    pr_current.init(pr_current_params);
    communication.sync.initSlave(TWIST_v_1_1_4);
#endif

    // Then declare tasks
    uint32_t app_task_number = task.createBackground(loop_application_task);
    uint32_t com_task_number = task.createBackground(loop_communication_task);
    task.createCritical(loop_critical_task, 100); // Uncomment if you use the critical task

    // Finally, start tasks
    task.startBackground(app_task_number);
    task.startBackground(com_task_number);
    task.startCritical(); // Uncomment if you use the critical task
}

//--------------LOOP FUNCTIONS--------------------------------

void loop_communication_task()
{
    while (1)
    {
        received_serial_char = console_getchar();
        switch (received_serial_char)
        {
        case 'h':
            //----------SERIAL INTERFACE MENU-----------------------
            printk(" ________________________________________\n");
            printk("|     ----AC client/server: %s ---       |\n", STR_ROLE);
            printk("|     press i : idle mode                |\n");
            printk("|     press p : power mode               |\n");
            printk("|________________________________________|\n\n");
            //------------------------------------------------------
            break;
        case 'i':
            printk("idle mode\n");
            mode = IDLEMODE;
            counter = 0;
            break;
        case 'p':
            if (is_downloading == false) {
                printk("power mode\n");
                mode = POWERMODE;
            }
            break;
#ifdef SERVER
        case 'l':
            k_gain += 0.1;
            break;
        case 'm':
            k_gain -= 0.1;
            break;
#endif
        case 'r':
            is_downloading = true;
            break;
        default:
            break;
        }
    }

}

/**
 * This is the code loop of the background task
 * It is executed second as defined by it suspend task in its last line.
 * You can use it to execute slow code such as state-machines.
 */
void loop_application_task()
{
    if (mode == POWERMODE)
    {

#ifndef SERVER
        printk("%i:", status);
        printk("%f:", Iref);
#endif
        printk("%f:", duty_cycle);
        printk("%f:", Vgrid);
        printk("%f:", I2_low_value);
        printk("%f:", I1_low_value);
        printk("%f:\n", V1_low_value);
    }
    
if (is_downloading) {
    printk("begin record\n");
    for (uint16_t k_download = 0;k_download < (RECORD_SIZE*NB_CURVES); k_download++)
    {
        printk("%08x\n", *((uint32_t *)record_array + k_download));
        task.suspendBackgroundMs(1);
    }
    printk("end record\n");
    is_downloading = false;
}
    task.suspendBackgroundMs(100);
}

/**
 * This is the code loop of the critical task
 * It is executed every 500 micro-seconds defined in the setup_software function.
 * You can use it to execute an ultra-fast code with the highest priority which cannot be interruped.
 * It is from it that you will control your power flow.
 */
void loop_critical_task()
{

    meas_data = data.getLatest(I1_LOW);
    if (meas_data < 10000 && meas_data > -10000)
        I1_low_value = meas_data;

    meas_data = data.getLatest(V1_LOW);
    if (meas_data < 10000 && meas_data > -10000)
        V1_low_value = meas_data;

    meas_data = data.getLatest(V2_LOW);
    if (meas_data < 10000 && meas_data > -10000)
        V2_low_value = meas_data;

    meas_data = data.getLatest(I2_LOW);
    if (meas_data < 10000 && meas_data > -10000)
        I2_low_value = meas_data;

    meas_data = data.getLatest(V_HIGH);
    if (meas_data != -10000)
        V_high = meas_data;

#ifdef SERVER

    if (mode == IDLEMODE)
    {
        if (pwm_enable == true)
        {
            twist.stopAll();
            spin.led.turnOff();
            k_gain = 1.0;
            tx_consigne.id_and_status = (1 << 6) + 0;
            communication.rs485.startTransmission();
        }

        pwm_enable = false;
    }
    else if (mode == POWERMODE)
    {
        /* Set POWER ON */

        angle += w0 * Ts;
        angle = ot_modulo_2pi(angle);

        Vgrid = Vgrid_amplitude * ot_sin(angle);
        duty_cycle = 0.5 + pr_voltage.calculateWithReturn(Vgrid, V1_low_value - V2_low_value) / (2.0 * Udc);

        twist.setAllDutyCycle(duty_cycle);

        if (counter == 0)
            tx_consigne.id_and_status = (1 << 6) + 2;
        else
            tx_consigne.id_and_status = (1 << 6) + 1;

        Iref = k_gain * I1_low_value;
        tx_consigne.Vref_fromSERVER = Vgrid;
        tx_consigne.Iref_fromSERVER = Iref;
        tx_consigne.w0_fromSERVER = w0;

        communication.rs485.startTransmission();

        if (counter_time % 4 == 0)
        {
            record_array[counter].I_low = I1_low_value;
            record_array[counter].V_low = V1_low_value;
            record_array[counter].Vhigh_value = V_high;
            record_array[counter].duty_cycle = duty_cycle;
            record_array[counter].Iref = Iref;
            record_array[counter].Vgrid = Vgrid;
            record_array[counter].angle = angle;
            if (counter < (RECORD_SIZE-1))
                counter++;
        }

        if (!pwm_enable)
        {
            pwm_enable = true;
            spin.led.turnOn();
            twist.startAll();
        }


        counter_time++;
    }

#endif

    ////
#ifndef SERVER ////// CLIENT

    if ((((status & 0x3) == 1 || (status & 0x3) == 2)))
    {
        mode = POWERMODE;
        pr_value = pr_current.calculateWithReturn(Iref, I1_low_value);
        duty_cycle = (Vgrid + pr_value )/(2*Udc) + 0.5;

        twist.setAllDutyCycle(duty_cycle);

        if (!pwm_enable)
        {
            pwm_enable = true;
            spin.led.turnOn();
            twist.startAll();
        }

        if (counter_time % 4 == 0)
        {
            record_array[counter].I_low = I1_low_value;
            record_array[counter].V_low = V1_low_value;
            record_array[counter].Vhigh_value = V_high;
            record_array[counter].duty_cycle = duty_cycle;
            record_array[counter].Iref = Iref;
            record_array[counter].Vgrid = Vgrid;
            record_array[counter].angle = pr_value;
            if (counter < (RECORD_SIZE-1))
                counter++;
        }
        counter_time++;
    }
    else
    {
        mode = IDLEMODE;
        if (pwm_enable == true)
        {
            twist.stopAll();
            spin.led.turnOff();
            pwm_enable = false;
        }
    }

#endif

}

/**
 * This is the main function of this example
 * This function is generic and does not need editing.
 */
int main(void)
{
    setup_routine();

    return 0;
}
