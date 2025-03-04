/*
 * Control Interface Task
 *
 * Determine communication mode (WiFi or Serial)
 *
 * Manage user controls:
 *   WiFi Reset Button
 *   Red/Green Dual Status LED
 *
 * Copyright 2020-2021 Dan Julio
 *
 * This file is part of tCam.
 *
 * tCam is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * tCam is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with tCam.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#include <stdbool.h>
#include "wifi_cmd_task.h"
#include "ctrl_task.h"
#include "rsp_task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ps_utilities.h"
#include "system_config.h"
#include "sys_utilities.h"
#include "wifi_utilities.h"



//
// Control Task Constants
//

// LED Colors
#define CTRL_LED_OFF 0
#define CTRL_LED_RED 1
#define CTRL_LED_YEL 2
#define CTRL_LED_GRN 3

// LED State Machine states
#define CTRL_LED_ST_SOLID      0
#define CTRL_LED_ST_BLINK_ON   1
#define CTRL_LED_ST_BLINK_OFF  2
#define CTRL_LED_ST_RST_ON     3
#define CTRL_LED_ST_RST_OFF    4
#define CTRL_LED_ST_FLT_ON     5
#define CTRL_LED_ST_FLT_OFF    6
#define CTRL_LED_ST_FLT_IDLE   7
#define CTRL_LED_ST_FW_REQ_R   8
#define CTRL_LED_ST_FW_REQ_G   9
#define CTRL_LED_ST_FW_UPD_ON  10
#define CTRL_LED_ST_FW_UPD_OFF 11

// Control State Machine states
#define CTRL_ST_STARTUP            0
#define CTRL_ST_WIFI_NOT_CONNECTED 1
#define CTRL_ST_WIFI_CONNECTED     2
#define CTRL_ST_CLIENT_CONNECTED   3
#define CTRL_ST_RESET_ALERT        4
#define CTRL_ST_RESET_ACTION       5
#define CTRL_ST_FAULT              6
#define CTRL_ST_FW_UPD_REQUEST     7
#define CTRL_ST_FW_UPD_PROCESS     8


//
// Control Task variables
//
static const char* TAG = "ctrl_task";

// State
static bool ctrl_ser_mode;
static int ctrl_state;
static int ctrl_pre_activity_state;
static int ctrl_led_state;
static int ctrl_action_timer;
static int ctrl_led_timer;
static int ctrl_fault_led_count;
static int ctrl_fault_type = CTRL_FAULT_NONE;



//
// Forward Declarations for internal functions
//
static void ctrl_task_init();
static void ctrl_debounce_button(bool* short_p, bool* long_p);
static void ctrl_set_led(int color);
static void ctrl_eval_sm();
static void ctrl_set_state(int new_st);
static void ctrl_eval_led_sm();
static void ctrl_set_led_state(int new_st);
static void ctrl_handle_notifications();



//
// API
//
void ctrl_task()
{
	ESP_LOGI(TAG, "Start task");
	
	ctrl_task_init();
	
	while (1) {
		vTaskDelay(pdMS_TO_TICKS(CTRL_EVAL_MSEC));
		ctrl_handle_notifications();
		ctrl_eval_led_sm();
		ctrl_eval_sm();
	}
}


bool ctrl_get_ser_mode()
{
	return ctrl_ser_mode;
}


// Not protected by semaphore since it won't be accessed until after subsequent notification
void ctrl_set_fault_type(int f)
{
	ctrl_fault_type = f;
	
	if (f == CTRL_FAULT_NONE) {
		// Asynchronously notify this task to return to the previous state
		xTaskNotify(task_handle_ctrl, CTRL_NOTIFY_FAULT_CLEAR, eSetBits);
	} else {
		// Save the existing state to return to when the fault is cleared
		if (ctrl_state != CTRL_ST_FW_UPD_PROCESS) {
			ctrl_pre_activity_state = ctrl_state;
		}
		
		// Asynchronously notify this task
		xTaskNotify(task_handle_ctrl, CTRL_NOTIFY_FAULT, eSetBits);
	}
}



//
// Internal functions
//
static void ctrl_task_init()
{
	// First, determine the operating mode
	gpio_reset_pin(MODE_SENSE_IO);
	gpio_set_direction(MODE_SENSE_IO, GPIO_MODE_INPUT);
	gpio_set_pull_mode(MODE_SENSE_IO, GPIO_PULLUP_ONLY);
	ctrl_ser_mode = gpio_get_level(MODE_SENSE_IO) == 0;
	
	// Setup the GPIO
	gpio_reset_pin(BTN_IO);
	gpio_set_direction(BTN_IO, GPIO_MODE_INPUT);
	gpio_pullup_en(BTN_IO);
	
	gpio_reset_pin(RED_LED_IO);
	gpio_set_direction(RED_LED_IO, GPIO_MODE_OUTPUT);
	gpio_set_drive_capability(RED_LED_IO, GPIO_DRIVE_CAP_3);
	
	gpio_reset_pin(GREEN_LED_IO);
	gpio_set_direction(GREEN_LED_IO, GPIO_MODE_OUTPUT);
	gpio_set_drive_capability(GREEN_LED_IO, GPIO_DRIVE_CAP_3);
	
	// Initialize state
	ctrl_set_state(CTRL_ST_STARTUP);
}


static void ctrl_debounce_button(bool* short_p, bool* long_p)
{
	// Button press state
	static bool prev_btn = false;
	static bool btn_down = false;
	static int btn_timer;
	
	// Dynamic variables
	bool cur_btn;
	bool btn_released = false;
	
	// Outputs will be set as necessary
	*short_p = false;
	*long_p = false;
	
	// Get current button value
	cur_btn = gpio_get_level(BTN_IO) == 0;
	
	// Evaluate button logic
	if (cur_btn && prev_btn && !btn_down) {
		// Button just pressed
		btn_down = true;
		btn_timer = CTRL_BTN_PRESS_MSEC / CTRL_EVAL_MSEC;
	}
	if (!cur_btn && !prev_btn && btn_down) {
		btn_down = false;
		btn_released = true;
	}
	prev_btn = cur_btn;
	
	// Evaluate timer for long press detection
	if (btn_down) {
		if (btn_timer != 0) {
			if (--btn_timer == 0) {
				// Long press detected
				*long_p = true;
			}
		}
	}
	
	if (btn_released && (btn_timer != 0)) {
		// Short press detected
		*short_p = true;
	}
}


static void ctrl_set_led(int color)
{
	switch (color) {
		case CTRL_LED_OFF:
			gpio_set_level(RED_LED_IO, 0);
			gpio_set_level(GREEN_LED_IO, 0);
			break;
		
		case CTRL_LED_RED:
			gpio_set_level(RED_LED_IO, 1);
			gpio_set_level(GREEN_LED_IO, 0);
			break;
		
		case CTRL_LED_YEL:
			gpio_set_level(RED_LED_IO, 1);
			gpio_set_level(GREEN_LED_IO, 1);
			break;
		
		case CTRL_LED_GRN:
			gpio_set_level(RED_LED_IO, 0);
			gpio_set_level(GREEN_LED_IO, 1);
			break;
	}
}


static void ctrl_eval_sm()
{
	bool btn_short_press;
	bool btn_long_press;
	wifi_info_t* wifi_info;
	
	// Look for button presses
	ctrl_debounce_button(&btn_short_press, &btn_long_press);
	
	switch (ctrl_state) {
		case CTRL_ST_STARTUP:
			// Wait to be taken out of this state by a notification
			break;

		case CTRL_ST_WIFI_NOT_CONNECTED:
			wifi_info = wifi_get_info();
			if (btn_long_press) {
				ctrl_set_state(CTRL_ST_RESET_ALERT);
			} else if ((wifi_info->flags & WIFI_INFO_FLAG_CONNECTED) == WIFI_INFO_FLAG_CONNECTED) {
				ctrl_set_state(CTRL_ST_WIFI_CONNECTED);
			}
			break;
		
		case CTRL_ST_WIFI_CONNECTED:
			wifi_info = wifi_get_info();
			if (btn_long_press) {
				ctrl_set_state(CTRL_ST_RESET_ALERT);
			} else if ((wifi_info->flags & WIFI_INFO_FLAG_CONNECTED) != WIFI_INFO_FLAG_CONNECTED) {
				ctrl_set_state(CTRL_ST_WIFI_NOT_CONNECTED);
			} else if (wifi_cmd_connected()) {
				ctrl_set_state(CTRL_ST_CLIENT_CONNECTED);
			}
			break;
		
		case CTRL_ST_CLIENT_CONNECTED:
			if (!ctrl_ser_mode) {
				if (btn_long_press) {
					ctrl_set_state(CTRL_ST_RESET_ALERT);
				} else if (!wifi_cmd_connected()) {
					// Goto wifi not connected in case this was why we lost our client.
					// If it is connected then we'll quickly go to wifi connected.
					ctrl_set_state(CTRL_ST_WIFI_NOT_CONNECTED);
				}
			}
			break;
		
		case CTRL_ST_RESET_ALERT:
			if (--ctrl_action_timer == 0) {
				// Reset alert done - initiate actual wifi reset
				ctrl_set_state(CTRL_ST_RESET_ACTION);
			}
			break;
		
		case CTRL_ST_RESET_ACTION:
			wifi_info = wifi_get_info();
			if ((wifi_info->flags & WIFI_INFO_FLAG_ENABLED) == WIFI_INFO_FLAG_ENABLED) {
				if (ctrl_fault_type == CTRL_FAULT_NONE) {
					ctrl_set_state(CTRL_ST_WIFI_NOT_CONNECTED);
				} else {
					// Return to previous fault indication to encourage user to power-cycle since
					// we may be halted in main due to the original error
					ctrl_set_state(CTRL_ST_FAULT);
				}
			}
			break;
		
		case CTRL_ST_FAULT:
			// Remain here until taken out if the fault is cleared or the user resets the wifi (wifi mode)
			if (!ctrl_ser_mode) {
				if (btn_long_press) {
					ctrl_set_state(CTRL_ST_RESET_ALERT);
				}
			}
			break;
		
		case CTRL_ST_FW_UPD_REQUEST:
			if (btn_short_press) {
				// Notify rsp_task user has authorized fw update
				xTaskNotify(task_handle_rsp, RSP_NOTIFY_FW_UPD_EN_MASK, eSetBits);
			}
			break;
		
		case CTRL_ST_FW_UPD_PROCESS:
			if (btn_short_press) {
				// Notify rsp_task user has terminated fw update
				xTaskNotify(task_handle_rsp, RSP_NOTIFY_FW_UPD_END_MASK, eSetBits);
			}
			break;
		
		default:
			ctrl_set_state(CTRL_ST_WIFI_NOT_CONNECTED);
	}
}


static void ctrl_eval_led_sm()
{
	switch (ctrl_led_state) {
		case CTRL_LED_ST_SOLID:
			// Wait to be taken out of this state
			break;
		
		case CTRL_LED_ST_BLINK_ON:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_BLINK_OFF);
			}
			break;
		
		case CTRL_LED_ST_BLINK_OFF:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_BLINK_ON);
			}
			break;
			
		case CTRL_LED_ST_RST_ON:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_RST_OFF);
			}
			break;
		
		case CTRL_LED_ST_RST_OFF:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_RST_ON);
			}
			break;
		
		case CTRL_LED_ST_FLT_ON:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_FLT_OFF);
			}
			break;
		
		case CTRL_LED_ST_FLT_OFF:
			if (--ctrl_led_timer == 0) {
				if (--ctrl_fault_led_count == 0) {
					ctrl_set_led_state(CTRL_LED_ST_FLT_IDLE);
				} else {
					ctrl_set_led_state(CTRL_LED_ST_FLT_ON);
				}
			}
			break;
		
		case CTRL_LED_ST_FLT_IDLE:
			if (--ctrl_led_timer == 0) {
				ctrl_fault_led_count = ctrl_fault_type;
				ctrl_set_led_state(CTRL_LED_ST_FLT_ON);
			}
			break;
			
		case CTRL_LED_ST_FW_REQ_R:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_FW_REQ_G);
			}
			break;
		
		case CTRL_LED_ST_FW_REQ_G:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_FW_REQ_R);
			}
			break;
		
		case CTRL_LED_ST_FW_UPD_ON:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_FW_UPD_OFF);
			}
			break;
		
		case CTRL_LED_ST_FW_UPD_OFF:
			if (--ctrl_led_timer == 0) {
				ctrl_set_led_state(CTRL_LED_ST_FW_UPD_ON);
			}
			break;
	}
}


static void ctrl_set_state(int new_st)
{
	ctrl_state = new_st;
	
	switch (new_st) {
		case CTRL_ST_STARTUP:
			ctrl_set_led(CTRL_LED_RED);
			ctrl_set_led_state(CTRL_LED_ST_SOLID);
			break;
		
		case CTRL_ST_WIFI_NOT_CONNECTED:
			// Start a slow blink
			ctrl_set_led_state(CTRL_LED_ST_BLINK_ON);
			break;
			
		case CTRL_ST_WIFI_CONNECTED:
			ctrl_set_led(CTRL_LED_YEL);
			ctrl_set_led_state(CTRL_LED_ST_SOLID);
			break;
		
		case CTRL_ST_CLIENT_CONNECTED:
			ctrl_set_led(CTRL_LED_GRN);
			ctrl_set_led_state(CTRL_LED_ST_SOLID);
			break;
		
		case CTRL_ST_RESET_ALERT:
			// Indicate wifi restart triggered
			ctrl_set_led_state(CTRL_LED_ST_RST_ON);
			ctrl_action_timer = CTRL_RESET_ALERT_MSEC / CTRL_EVAL_MSEC;
			break;
		
		case CTRL_ST_RESET_ACTION:
			// Re-initialize the wifi info in persistent storage
			if (ps_reinit_wifi()) {
				// Attempt to re-initialize the wifi stack
				if (!wifi_reinit()) {
					// Change to fault state
					ctrl_set_led_state(CTRL_LED_ST_FLT_ON);
					ctrl_state = CTRL_ST_FAULT;
				}
			} else {
				// Change to fault state
				ctrl_set_led_state(CTRL_LED_ST_FLT_ON);
				ctrl_state = CTRL_ST_FAULT;
			}
			break;
		
		case CTRL_ST_FAULT:
			if (ctrl_fault_type != CTRL_FAULT_NONE) {
				// Setup to blink fault code
				ctrl_fault_led_count = ctrl_fault_type;
				ctrl_set_led_state(CTRL_LED_ST_FLT_ON);
			}
			break;
		
		case CTRL_ST_FW_UPD_REQUEST:
			// Start alternating red/green blink to indicate fw update request
			ctrl_set_led_state(CTRL_LED_ST_FW_REQ_R);
			break;
		
		case CTRL_ST_FW_UPD_PROCESS:
			// Start fast green blink to indicate fw update is in progress
			ctrl_set_led_state(CTRL_LED_ST_FW_UPD_ON);
			break;
	}
}


static void ctrl_set_led_state(int new_st)
{
	ctrl_led_state = new_st;
	
	switch (new_st) {
		case CTRL_LED_ST_SOLID:
			// LED Color set outside this call
			break;
		
		case CTRL_LED_ST_BLINK_ON:
			ctrl_led_timer = CTRL_SLOW_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_YEL);
			break;
		
		case CTRL_LED_ST_BLINK_OFF:
			ctrl_led_timer = CTRL_SLOW_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_OFF);
			break;
		
		case CTRL_LED_ST_RST_ON:
			ctrl_led_timer = CTRL_FAST_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_YEL);
			break;
		
		case CTRL_LED_ST_RST_OFF:
			ctrl_led_timer = CTRL_FAST_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_OFF);
			break;
		
		case CTRL_LED_ST_FLT_ON:
			ctrl_led_timer = CTRL_FAULT_BLINK_ON_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_RED);
			break;
		
		case CTRL_LED_ST_FLT_OFF:
			ctrl_led_timer = CTRL_FAULT_BLINK_OFF_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_OFF);
			break;
			
		case CTRL_LED_ST_FLT_IDLE:
			ctrl_led_timer = CTRL_FAULT_IDLE_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_OFF);
			break;
			
		case CTRL_LED_ST_FW_REQ_R:
			ctrl_led_timer = CTRL_FW_UPD_REQ_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_RED);
			break;
			
		case CTRL_LED_ST_FW_REQ_G:
			ctrl_led_timer = CTRL_FW_UPD_REQ_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_GRN);
			break;

		case CTRL_LED_ST_FW_UPD_ON:
			ctrl_led_timer = CTRL_FAST_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_GRN);
			break;

		case CTRL_LED_ST_FW_UPD_OFF:
			ctrl_led_timer = CTRL_FAST_BLINK_MSEC / CTRL_EVAL_MSEC;
			ctrl_set_led(CTRL_LED_OFF);
			break;
	}
}


static void ctrl_handle_notifications()
{
	uint32_t notification_value;
	
	notification_value = 0;
	if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notification_value, 0)) {
		if (Notification(notification_value, CTRL_NOTIFY_STARTUP_DONE)) {
			if (ctrl_state != CTRL_ST_FAULT) {
				if (ctrl_ser_mode) {
					ctrl_set_state(CTRL_ST_CLIENT_CONNECTED);
				} else {
					ctrl_set_state(CTRL_ST_WIFI_NOT_CONNECTED);
				}
			}
		}
		
		if (Notification(notification_value, CTRL_NOTIFY_FAULT)) {
			ctrl_set_state(CTRL_ST_FAULT);
		}
		
		if (Notification(notification_value, CTRL_NOTIFY_FAULT_CLEAR)) {
			// Return to the pre-fault state
			ctrl_set_state(ctrl_pre_activity_state);
		}
		
		if (Notification(notification_value, CTRL_NOTIFY_FW_UPD_REQ)) {
			// Save the existing state to return to when the update is cleared
			if (ctrl_state != CTRL_ST_FAULT) {
				ctrl_pre_activity_state = ctrl_state;
			}
			ctrl_set_state(CTRL_ST_FW_UPD_REQUEST);
		}
		
		if (Notification(notification_value, CTRL_NOTIFY_FW_UPD_PROCESS)) {
			ctrl_set_state(CTRL_ST_FW_UPD_PROCESS);
		}

		if (Notification(notification_value, CTRL_NOTIFY_FW_UPD_DONE)) {
			// Return to the pre-fault state
			ctrl_set_state(ctrl_pre_activity_state);
		}
		
		if (Notification(notification_value, CTRL_NOTIFY_FW_UPD_REBOOT)) {
			// Delay a bit to allow any final communication to occur and then reboot
			vTaskDelay(pdMS_TO_TICKS(500));
			esp_restart();
		}
	}
}

