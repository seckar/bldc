/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */
#include "ch.h"
#include "hal.h"
#include "terminal.h"
#include "mcpwm.h"
#include "mcpwm_foc.h"
#include "mcpwm_foc2.h"
#include "mc_interface.h"
#include "commands.h"
#include "hw.h"
#include "comm_can.h"
#include "utils.h"
#include "timeout.h"
#include "encoder.h"
#include "drv8301.h"
#include "drv8305.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

// Settings
#define FAULT_VEC_LEN						25
#define CALLBACK_LEN						25

// Private types
typedef struct _terminal_callback_struct {
	const char *command;
	const char *help;
	const char *arg_names;
	void (*cbf)(int argc, const char **argv);
} terminal_callback_struct;

// Private variables
static volatile fault_data fault_vec[FAULT_VEC_LEN];
static volatile int fault_vec_write = 0;
static volatile fault_data fault_vec2[FAULT_VEC_LEN];
static volatile int fault_vec_write2 = 0;
static terminal_callback_struct callbacks[CALLBACK_LEN];
static int callback_write = 0;

void terminal_process_string(char *str) {
	enum {
		kMaxArgs = 64
	};
	int argc = 0;
	char *argv[kMaxArgs];

	char *p2 = strtok(str, " ");
	while (p2 && argc < kMaxArgs) {
		argv[argc++] = p2;
		p2 = strtok(0, " ");
	}

	if (argc == 0) {
		commands_printf("No command received\n");
		return;
	}

	mc_configuration mcconf; // static to save some stack
	mc_configuration mcconf2; // static to save some stack
	mc_configuration mcconf_old; // static to save some stack
	mc_configuration mcconf_old2; // static to save some stack
	mcconf = *mc_interface_get_configuration();
	mcconf2 = *mc_interface_get_configuration2();
	mcconf_old = mcconf;
	mcconf_old2 = mcconf2;

	if (strcmp(argv[0], "ping") == 0) {
		commands_printf("pong\n");
	}
	else if (strcmp(argv[0], "calibrate") == 0) {
		commands_printf("Measuring R and L...\n");
		float r1 = 0;
		float l1 = 0;
		float r2 = 0;
		float l2 = 0;
		mcconf = *mc_interface_get_configuration();
		mcconf_old = mcconf;
		mcconf2 = *mc_interface_get_configuration2();
		mcconf_old2 = mcconf2;
		mcconf.motor_type = MOTOR_TYPE_FOC;
		mcconf2.motor_type = MOTOR_TYPE_FOC;
		mc_interface_set_configuration(&mcconf, &mcconf2);
		mcpwm_foc_measure_res_ind(&r1, &l1, &r2, &l2);
		mc_interface_set_configuration(&mcconf_old, &mcconf_old2);
		if (r1 > 0.0001 && r1 < 5 && l1 > 0.1 && l1 < 1e3) {
			commands_printf("Completed Successfully R1: %f mOhm L1: %f uH", (double)(r1 * 1e3), (double)(l1));
			mcconf.foc_motor_r = r1;
			mcconf.foc_motor_l = l1 * 1e-6;
			static float tc = 1000.0;
			static float bw;
			bw = 1.0 / (tc * 1e-6);
			static float kp, ki;
			l1 /= 1e6;
			kp = l1 * bw;
			ki = r1 * bw;
			commands_printf("Applying current gains kp: %f ki: %f", (double)(kp), (double)(ki));
			mcconf.foc_current_kp = kp;
			mcconf.foc_current_ki = ki;
		}
		else {
			commands_printf("failed! R1: %f mOhm L1: %f uH", (double)(r1 * 1e3), (double)(l1));
		}
		if (r2 > 0.0001 && r2 < 5 && l2 > 0.1 && l2 < 1e3) {
			commands_printf("Completed Successfully R2: %f mOhm L2: %f uH", (double)(r2 * 1e3), (double)(l2));
			mcconf2.foc_motor_r = r2;
			mcconf2.foc_motor_l = l2 * 1e-6;
			static float tc = 1000.0;
			static float bw;
			bw = 1.0 / (tc * 1e-6);
			static float kp2, ki2;
			l2 /= 1e6;
			kp2 = l2 * bw;
			ki2 = r2 * bw;
			commands_printf("Applying current gains kp: %f ki: %f", (double)(kp2), (double)(ki2));
			mcconf2.foc_current_kp = kp2;
			mcconf2.foc_current_ki = ki2;
		}
		else {
			commands_printf("failed! R2: %f mOhm L2: %f uH", (double)(r2 * 1e3), (double)(l2));
		}
		commands_printf("\nCalibrating Hall Sensors (if connected)...\n");

		mcconf_old = mcconf;
		mcconf_old2 = mcconf2;
		mcconf.motor_type = MOTOR_TYPE_FOC;
		mcconf.m_sensor_port_mode = SENSOR_PORT_MODE_HALL;
		mcconf2.m_sensor_port_mode = SENSOR_PORT_MODE_HALL;
		mcconf.foc_f_sw = 20000.0;
		mcconf.foc_current_kp = 0.01;
		mcconf.foc_current_ki = 10.0;
		mcconf2.motor_type = MOTOR_TYPE_FOC;
		mcconf2.foc_f_sw = 20000.0;
		mcconf2.foc_current_kp = 0.01;
		mcconf2.foc_current_ki = 10.0;
		mc_interface_set_configuration(&mcconf, &mcconf2);
		uint8_t hall_tab[16];
		float current = 12.0;
		mcpwm_foc_hall_detect(current, hall_tab);
		mcconf = mcconf_old;
		mcconf2 = mcconf_old2;
		int bad_sectors1 = 0;
		int bad_sectors2 = 0;
		for (int i = 0; i < 8; i++) {
			if (hall_tab[i] > 200) {
				bad_sectors1++;
			}
			if (hall_tab[i + 8] > 200) {
				bad_sectors2++;
			}
		}
		if (bad_sectors1 == 2) {
			mcconf.m_sensor_port_mode = SENSOR_PORT_MODE_HALL;
			mcconf.foc_sensor_mode = FOC_SENSOR_MODE_HALL;
			for (int i = 0; i < 8; i++) {
				mcconf.foc_hall_table[i] = hall_tab[i];
			}
			commands_printf("Hall sensor 1 successfully Detected:");
			commands_printf("%i %i %i %i %i %i %i %i", hall_tab[0], hall_tab[1], hall_tab[2], hall_tab[3], hall_tab[4], hall_tab[5], hall_tab[6], hall_tab[7]);
		}
		else {
			mcconf.foc_sensor_mode = FOC_SENSOR_MODE_SENSORLESS;
			commands_printf("Hall sensor 1 not functioning:");
			commands_printf("%i %i %i %i %i %i %i %i", hall_tab[0], hall_tab[1], hall_tab[2], hall_tab[3], hall_tab[4], hall_tab[5], hall_tab[6], hall_tab[7]);
		}
		if (bad_sectors2 == 2) {
			mcconf2.m_sensor_port_mode = SENSOR_PORT_MODE_HALL;
			mcconf2.foc_sensor_mode = FOC_SENSOR_MODE_HALL;
			for (int i = 0; i < 8; i++) {
				mcconf2.foc_hall_table[i] = hall_tab[i+8];
			}
			commands_printf("Hall sensor 2 successfully Detected:");
			commands_printf("%i %i %i %i %i %i %i %i", hall_tab[8], hall_tab[9], hall_tab[10], hall_tab[11], hall_tab[12], hall_tab[13], hall_tab[14], hall_tab[15]);
		}
		else {
			mcconf2.foc_sensor_mode = FOC_SENSOR_MODE_SENSORLESS;
			commands_printf("Hall sensor 2 not functioning:");
			commands_printf("%i %i %i %i %i %i %i %i", hall_tab[8], hall_tab[9], hall_tab[10], hall_tab[11], hall_tab[12], hall_tab[13], hall_tab[14], hall_tab[15]);
		}
		mc_interface_set_configuration(&mcconf, &mcconf2);

		commands_printf("\nMeasuring Lambda, spin each motor in the direction you wish to be forwards (times out in 15 seconds)...\n");
		float linkage1 = 0;
		float linkage2 = 0;
		bool dir1 = 0;
		bool dir2 = 0;
		conf_general_measure_flux_linkage(&linkage1, &linkage2, &dir1, &dir2);
		if (linkage1 > 1e-12 && linkage1 < 0.1) {
			mcconf.foc_motor_flux_linkage = linkage1;
			mcconf.m_invert_direction = !dir1;
			static float obsGain;
			obsGain = (0.001 / (linkage1 * linkage1));
			mcconf.foc_observer_gain = obsGain*1e6;
			commands_printf("Linkage 1 set to: %f mWb", (double)(1000.0 * linkage1));
			commands_printf("Observer gain 1 set to: %f", (double)obsGain);
		}
		else {
			commands_printf("Linkage 1 value invalid: %f mWb", (double)(1000.0 * linkage1));
		}
		if (linkage2 > 1e-12 && linkage2 < 0.1) {
			mcconf2.foc_motor_flux_linkage = linkage2;
			mcconf2.m_invert_direction = !dir2;
			static float obsGain2;
			obsGain2 = (0.001 / (linkage2 * linkage2));
			mcconf2.foc_observer_gain = obsGain2*1e6;
			commands_printf("Linkage 2 set to: %f mWb", (double)(1000.0 * linkage2));
			commands_printf("Observer gain 2 set to: %f", (double)obsGain2);
		}
		else {
			commands_printf("Linkage 2 value invalid: %f mWb", (double)(1000.0 * linkage2));
		}
		commands_printf("\nApplying Calibration...\n");
		conf_general_store_mc_configuration(&mcconf, &mcconf2);
		mc_interface_set_configuration(&mcconf, &mcconf2);
		commands_printf("Completed Successfully\n");

	}
	else if (strcmp(argv[0], "stop") == 0) {
		mc_interface_set_duty(0, 0);
		commands_printf("Motor stopped\n");
	}
	else if (strcmp(argv[0], "turnFWD") == 0) {
	          mcpwm_foc_one_turn_openloop(10.0, 1);
	}
	else if (strcmp(argv[0], "turnREV") == 0) {
		     mcpwm_foc_one_turn_openloop(10.0, 0);
	}
	else if (strcmp(argv[0], "last_adc_duration") == 0) {
		commands_printf("Latest ADC duration: %.4f ms", (double)(mcpwm_get_last_adc_isr_duration() * 1000.0));
		commands_printf("Latest injected ADC duration: %.4f ms", (double)(mc_interface_get_last_inj_adc_isr_duration() * 1000.0));
		commands_printf("Latest sample ADC duration: %.4f ms\n", (double)(mc_interface_get_last_sample_adc_isr_duration() * 1000.0));
	}
	else if (strcmp(argv[0], "kv") == 0) {
		commands_printf("Calculated KV: %.2f rpm/volt\n", (double)mcpwm_get_kv_filtered());
	}
	else if (strcmp(argv[0], "mem") == 0) {
		size_t n, size;
		n = chHeapStatus(NULL, &size);
		commands_printf("core free memory : %u bytes", chCoreGetStatusX());
		commands_printf("heap fragments   : %u", n);
		commands_printf("heap free total  : %u bytes\n", size);
	}
	else if (strcmp(argv[0], "threads") == 0) {
		thread_t *tp;
		static const char *states[] = {CH_STATE_NAMES};
		commands_printf("    addr    stack prio refs     state           name time    ");
		commands_printf("-------------------------------------------------------------");
		tp = chRegFirstThread();
		do {
			commands_printf("%.8lx %.8lx %4lu %4lu %9s %14s %lu", (uint32_t)tp, (uint32_t)tp->p_ctx.r13, (uint32_t)tp->p_prio, (uint32_t)(tp->p_refs - 1), states[tp->p_state], tp->p_name, (uint32_t)tp->p_time);
			tp = chRegNextThread(tp);
		} while (tp != NULL);
		commands_printf("");
	}
	else if (strcmp(argv[0], "fault") == 0) {
		commands_printf("%s\n", mc_interface_fault_to_string(mc_interface_get_fault()));
	}
	else if (strcmp(argv[0], "faults") == 0) {
		if (fault_vec_write == 0) {
			commands_printf("No faults registered since startup\n");
		}
		else {
			commands_printf("The following faults were registered since start:\n");
			for (int i = 0; i < fault_vec_write; i++) {
				commands_printf("Fault            : %s", mc_interface_fault_to_string(fault_vec[i].fault));
				commands_printf("Voltage          : %.2f", (double)fault_vec[i].voltage);
				commands_printf("Cycles running   : %d", fault_vec[i].cycles_running);
				commands_printf("TIM duty         : %d", (int)((float)fault_vec[i].tim_top * fault_vec[i].duty));
				commands_printf("TIM val samp     : %d", fault_vec[i].tim_val_samp);
				commands_printf("TIM current samp : %d", fault_vec[i].tim_current_samp);
				commands_printf("TIM top          : %d", fault_vec[i].tim_top);
				commands_printf("                   MOTOR1   MOTOR2");
				commands_printf("Current          : %.1f     %.1f", (double)fault_vec[i].current, (double)fault_vec[i].current2);
				commands_printf("Current filtered : %.1f     %.1f", (double)fault_vec[i].current_filtered, (double)fault_vec[i].current_filtered2);
				commands_printf("Duty             : %.3f     %.3f", (double)fault_vec[i].duty, (double)fault_vec[i].duty2);
				commands_printf("RPM              : %.1f     %.1f", (double)fault_vec[i].rpm, (double)fault_vec[i].rpm2);
				commands_printf("Tacho            : %d       %d", fault_vec[i].tacho,fault_vec[i].tacho2);
				commands_printf("Mosfet Temp      : %.2f     %.2f", (double)fault_vec[i].temperature, (double)fault_vec[i].temperature2);
#ifdef HW_HAS_DRV8301
				if (fault_vec[i].fault == FAULT_CODE_DRV || fault_vec[i].fault == FAULT_CODE_DRV2) {
					commands_printf("DRV8301_FAULTS_MOTOR1   : %s", drv8301_faults_to_string(fault_vec[i].drv8301_faults));
					commands_printf("DRV8301_FAULTS_MOTOR2   : %s", drv8301_faults_to_string(fault_vec[i].drv8301_faults2));
				}
#endif
				commands_printf(" ");
			}
		}
	}
	else if (strcmp(argv[0], "rpm") == 0) {
		commands_printf("Electrical RPM: %.2f rpm\n", (double)mc_interface_get_rpm());
	}
	else if (strcmp(argv[0], "tacho") == 0) {
		commands_printf("Tachometer counts: %i\n", mc_interface_get_tachometer_value(0));
	}
	else if (strcmp(argv[0], "tim") == 0) {
		chSysLock();
		volatile int t1_cnt = TIM1->CNT;
		volatile int t8_cnt = 2*TIM2->CNT ;//TIM8->CNT + TIM1->CNT;
		volatile int dir1 = !!(TIM1->CR1 & (1 << 4));
		volatile int dir8 = !!(TIM8->CR1 & (1 << 4));
		chSysUnlock();
		int duty1 = 2*TIM2->ARR;//TIM1->CCR1;
		int duty2 = TIM1->CCR2;
		int duty3 = TIM1->CCR3;
		int duty4 = TIM8->CCR1;
		int duty5 = TIM8->CCR2;
		int duty6 = TIM8->CCR3;
		int top = TIM1->ARR;
		int voltage1_samp = TIM2->CCR1;
		int voltage2_samp = TIM2->CCR2;
		int current1_samp = TIM1->CCR4;
		int current2_samp = TIM8->CCR4;
		commands_printf("Tim1 CNT: %i", t1_cnt);
		commands_printf("Tim8 CNT: %u", t8_cnt);
		commands_printf("Duty cycle1: %u", duty1);
		commands_printf("Duty cycle2: %u", duty2);
		commands_printf("Duty cycle3: %u", duty3);
		commands_printf("Duty cycle4: %u", duty4);
		commands_printf("Duty cycle5: %u", duty5);
		commands_printf("Duty cycle6: %u", duty6);
		commands_printf("Top: %u", top);
		commands_printf("Dir1: %u", dir1);
		commands_printf("Dir8: %u", dir8);
		commands_printf("Voltage 1 sample: %u", voltage1_samp);
		commands_printf("Voltage 2 sample: %u", voltage2_samp);
		commands_printf("Current 1 sample: %u", current1_samp);
		commands_printf("Current 2 sample: %u\n", current2_samp);
	}
	else if (strcmp(argv[0], "volt") == 0) {
		commands_printf("Input voltage: %.2f\n", (double)GET_INPUT_VOLTAGE());
	}
	else if (strcmp(argv[0], "param_detect") == 0) {
		// Use COMM_MODE_DELAY and try to figure out the motor parameters.
		if (argc == 4) {
			float current = -1.0;
			float min_rpm = -1.0;
			float low_duty = -1.0;
			sscanf(argv[1], "%f", &current);
			sscanf(argv[2], "%f", &min_rpm);
			sscanf(argv[3], "%f", &low_duty);

			if (current > 0.0 && current < mcconf.l_current_max && min_rpm > 10.0 && min_rpm < 3000.0 && low_duty > 0.02 && low_duty < 0.8) {

				float cycle_integrator;
				float coupling_k;
				int8_t hall_table[8];
				int hall_res;
				if (conf_general_detect_motor_param(current, min_rpm, low_duty, &cycle_integrator, &coupling_k, hall_table, &hall_res)) {
					commands_printf("Cycle integrator limit: %.2f", (double)cycle_integrator);
					commands_printf("Coupling factor: %.2f", (double)coupling_k);

					if (hall_res == 0) {
						commands_printf("Detected hall sensor table:");
						commands_printf("%i, %i, %i, %i, %i, %i, %i, %i\n", hall_table[0], hall_table[1], hall_table[2], hall_table[3], hall_table[4], hall_table[5], hall_table[6], hall_table[7]);
					}
					else if (hall_res == -1) {
						commands_printf("Hall sensor detection failed:");
						commands_printf("%i, %i, %i, %i, %i, %i, %i, %i\n", hall_table[0], hall_table[1], hall_table[2], hall_table[3], hall_table[4], hall_table[5], hall_table[6], hall_table[7]);
					}
					else if (hall_res == -2) {
						commands_printf("WS2811 enabled. Hall sensors cannot be used.\n");
					}
					else if (hall_res == -3) {
						commands_printf("Encoder enabled. Hall sensors cannot be used.\n");
					}
				}
				else {
					commands_printf("Detection failed. Try again with different parameters.\n");
				}
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires three arguments.\n");
		}
	}
	else if (strcmp(argv[0], "rpm_dep") == 0) {
		mc_rpm_dep_struct rpm_dep = mcpwm_get_rpm_dep();
		commands_printf("Cycle int limit: %.2f", (double)rpm_dep.cycle_int_limit);
		commands_printf("Cycle int limit running: %.2f", (double)rpm_dep.cycle_int_limit_running);
		commands_printf("Cycle int limit max: %.2f\n", (double)rpm_dep.cycle_int_limit_max);
	}
	else if (strcmp(argv[0], "can_devs") == 0) {
		commands_printf("CAN devices seen on the bus the past second:\n");
		for (int i = 0; i < CAN_STATUS_MSGS_TO_STORE; i++) {
			can_status_msg *msg = comm_can_get_status_msg_index(i);

			if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < 1.0) {
				commands_printf("ID                 : %i", msg->id);
				commands_printf("RX Time            : %i", msg->rx_time);
				commands_printf("Age (milliseconds) : %.2f", (double)(UTILS_AGE_S(msg->rx_time) * 1000.0));
				commands_printf("RPM                : %.2f", (double)msg->rpm);
				commands_printf("Current            : %.2f", (double)msg->current);
				commands_printf("Duty               : %.2f\n", (double)msg->duty);
			}
		}
	}
	else if (strcmp(argv[0], "foc_encoder_detect") == 0) {
		if (argc == 2) {
			float current = -1.0;
			sscanf(argv[1], "%f", &current);

			if (current > 0.0 && current <= mcconf.l_current_max) {
				if (encoder_is_configured()) {
					mc_motor_type type_old = mcconf.motor_type;
					mcconf.motor_type = MOTOR_TYPE_FOC;
					mc_interface_set_configuration(&mcconf, &mcconf);

					float offset = 0.0;
					float ratio = 0.0;
					bool inverted = false;
					mcpwm_foc_encoder_detect(current, true, &offset, &ratio, &inverted);

					mcconf.motor_type = type_old;
					mc_interface_set_configuration(&mcconf, &mcconf);

					commands_printf("Offset   : %.2f", (double)offset);
					commands_printf("Ratio    : %.2f", (double)ratio);
					commands_printf("Inverted : %s\n", inverted ? "true" : "false");
				}
				else {
					commands_printf("Encoder not enabled.\n");
				}
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
	}
	else if (strcmp(argv[0], "measure_res") == 0) {
		if (argc == 2) {
			float current = -1.0;
			sscanf(argv[1], "%f", &current);

			if (current > 0.0 && current <= mcconf.l_current_max) {
				mcconf.motor_type = MOTOR_TYPE_FOC;
				mc_interface_set_configuration(&mcconf, &mcconf);
				float res = 0;
				float res2 = 0;
				mcpwm_foc_measure_resistance(current, 2000, &res, &res2);

				commands_printf("Resistance: %.6f ohm   %.6f ohm\n", (double)res, (double)res2);

				mc_interface_set_configuration(&mcconf_old, &mcconf_old);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
	}
	else if (strcmp(argv[0], "measure_ind") == 0) {
		if (argc == 2) {
			float duty = -1.0;
			sscanf(argv[1], "%f", &duty);

			if (duty > 0.0 && duty < 0.9) {
				mcconf.motor_type = MOTOR_TYPE_FOC;
				mcconf.foc_f_sw = 3000.0;
				mc_interface_set_configuration(&mcconf, &mcconf);

				float curr, ind1, ind2;
				mcpwm_foc_measure_inductance(duty, 200, &curr, &ind1, &ind2);
				commands_printf("Inductance: %.2f microhenry %.2f microhenry (%.2f A)\n", (double)ind1, (double)ind2, (double)curr);

				mc_interface_set_configuration(&mcconf_old, &mcconf_old);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
	}
	else if (strcmp(argv[0], "measure_linkage") == 0) {
		if (argc == 5) {
			float current = -1.0;
			float duty = -1.0;
			float min_erpm = -1.0;
			float res = -1.0;
			float res2 = -1.0;
			sscanf(argv[1], "%f", &current);
			sscanf(argv[2], "%f", &duty);
			sscanf(argv[3], "%f", &min_erpm);
			sscanf(argv[4], "%f", &res);
			sscanf(argv[4], "%f", &res2);
			if (current > 0.0 && current <= mcconf.l_current_max && min_erpm > 0.0 && duty > 0.02 && res >= 0.0) {
				float linkage;
				float linkage2;
				bool dir1, dir2;
				conf_general_measure_flux_linkage(&linkage, &linkage2, &dir1, &dir2);
				commands_printf("Flux linkage: %.7f\n", (double)linkage);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
	}
	else if (strcmp(argv[0], "measure_res_ind") == 0) {
		mcconf.motor_type = MOTOR_TYPE_FOC;
		mc_interface_set_configuration(&mcconf, &mcconf);

		float res = 0.0;
		float ind = 0.0;
		float res2 = 0.0;
		float ind2 = 0.0;
		mcpwm_foc_measure_res_ind(&res, &ind, &res2, &ind2);
		commands_printf("Resistance: %.6f ohm", (double)res);
		commands_printf("Inductance: %.2f microhenry\n", (double)ind);

		mc_interface_set_configuration(&mcconf_old, &mcconf_old);
	}
	else if (strcmp(argv[0], "measure_linkage_foc") == 0) {
		if (argc == 2) {
			float duty = -1.0;
			sscanf(argv[1], "%f", &duty);

			if (duty > 0.0) {
				mcconf.motor_type = MOTOR_TYPE_FOC;
				mc_interface_set_configuration(&mcconf, &mcconf);
				const float res = (3.0 / 2.0) * mcconf.foc_motor_r;

				// Disable timeout
				systime_t tout = timeout_get_timeout_msec();
				float tout_c = timeout_get_brake_current();
				timeout_reset();
				timeout_configure(60000, 0.0);

				for (int i = 0; i < 100; i++) {
					mc_interface_set_duty(((float)i / 100.0) * duty, ((float)i / 100.0) * duty);
					chThdSleepMilliseconds(20);
				}

				float vq_avg = 0.0;
				float rpm_avg = 0.0;
				float samples = 0.0;
				float iq_avg = 0.0;
				for (int i = 0; i < 1000; i++) {
					vq_avg += mcpwm_foc_get_vq();
					rpm_avg += mc_interface_get_rpm();
					iq_avg += mc_interface_get_tot_current_directional();
					samples += 1.0;
					chThdSleepMilliseconds(1);
				}

				mc_interface_release_motor();

				mc_interface_set_configuration(&mcconf_old, &mcconf_old);

				// Enable timeout
				timeout_configure(tout, tout_c);

				vq_avg /= samples;
				rpm_avg /= samples;
				iq_avg /= samples;

				float linkage = (vq_avg - res * iq_avg) / (rpm_avg * ((2.0 * M_PI) / 60.0));

				commands_printf("Flux linkage: %.7f\n", (double)linkage);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
	}
	else if (strcmp(argv[0], "foc_state") == 0) {
		mcpwm_foc_print_state();
		commands_printf(" ");
	}
	else if (strcmp(argv[0], "hw_status") == 0) {
		commands_printf("Firmware: %d.%d", FW_VERSION_MAJOR, FW_VERSION_MINOR);
#ifdef HW_NAME
		commands_printf("Hardware: %s", HW_NAME);
#endif
		commands_printf("UUID: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
					 STM32_UUID_8[0],
					 STM32_UUID_8[1], STM32_UUID_8[2], STM32_UUID_8[3],
					 STM32_UUID_8[4],
					 STM32_UUID_8[5], STM32_UUID_8[6], STM32_UUID_8[7],
					 STM32_UUID_8[8],
					 STM32_UUID_8[9], STM32_UUID_8[10], STM32_UUID_8[11]);
		commands_printf("Permanent NRF found: %s", conf_general_permanent_nrf_found ? "Yes" : "No");
		commands_printf(" ");
	}
	else if (strcmp(argv[0], "drv8301_read_reg") == 0) {
#ifdef HW_HAS_DRV8301
		if (argc == 2) {
			int reg = -1;
			sscanf(argv[1], "%d", &reg);

			if (reg >= 0) {
				unsigned int res = drv8301_read_reg(reg);
				char bl[9];
				char bh[9];

				utils_byte_to_binary((res >> 8) & 0xFF, bh);
				utils_byte_to_binary(res & 0xFF, bl);

				commands_printf("Reg 0x%02x: %s %s (0x%04x)\n", reg, bh, bl, res);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
#else
		commands_printf("This hardware does not have a DRV8301.\n");
#endif
	}
	else if (strcmp(argv[0], "drv8301_write_reg") == 0) {
#ifdef HW_HAS_DRV8301
		if (argc == 3) {
			int reg = -1;
			int val = -1;
			sscanf(argv[1], "%d", &reg);
			sscanf(argv[2], "%x", &val);

			if (reg >= 0 && val >= 0) {
				drv8301_write_reg(reg, val);
				unsigned int res = drv8301_read_reg(reg);
				char bl[9];
				char bh[9];

				utils_byte_to_binary((res >> 8) & 0xFF, bh);
				utils_byte_to_binary(res & 0xFF, bl);

				commands_printf("New reg value 0x%02x: %s %s (0x%04x)\n", reg, bh, bl, res);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires two arguments.\n");
		}
#else
		commands_printf("This hardware does not have a DRV8301.\n");
#endif
	}
	else if (strcmp(argv[0], "drv8301_set_oc_adj") == 0) {
#ifdef HW_HAS_DRV8301
		if (argc == 2) {
			int val = -1;
			sscanf(argv[1], "%d", &val);

			if (val >= 0 && val < 32) {
				drv8301_set_oc_adj(val);
				unsigned int res = drv8301_read_reg(2);
				char bl[9];
				char bh[9];

				utils_byte_to_binary((res >> 8) & 0xFF, bh);
				utils_byte_to_binary(res & 0xFF, bl);

				commands_printf("New reg value 0x%02x: %s %s (0x%04x)\n", 2, bh, bl, res);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
#else
		commands_printf("This hardware does not have a DRV8301.\n");
#endif
	}
	else if (strcmp(argv[0], "drv8301_print_faults") == 0) {
#ifdef HW_HAS_DRV8301
		commands_printf(drv8301_faults_to_string(drv8301_read_faults()));
#else
		commands_printf("This hardware does not have a DRV8301.\n");
#endif
	}
	else if (strcmp(argv[0], "drv8301_reset_faults") == 0) {
#ifdef HW_HAS_DRV8301
		drv8301_reset_faults();
#else
		commands_printf("This hardware does not have a DRV8301.\n");
#endif
	}
	else if (strcmp(argv[0], "drv8305_read_reg") == 0) {
#ifdef HW_HAS_DRV8305
		if (argc == 2) {
			int reg = -1;
			sscanf(argv[1], "%d", &reg);

			if (reg >= 0) {
				unsigned int res = drv8305_read_reg(reg);
				char bl[9];
				char bh[9];

				utils_byte_to_binary((res >> 8) & 0xFF, bh);
				utils_byte_to_binary(res & 0xFF, bl);

				commands_printf("Reg 0x%02x: %s %s (0x%04x)\n", reg, bh, bl, res);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires one argument.\n");
		}
#else
		commands_printf("This hardware does not have a DRV8305.\n");
#endif
	}
	else if (strcmp(argv[0], "drv8305_write_reg") == 0) {
#ifdef HW_HAS_DRV8305
		if (argc == 3) {
			int reg = -1;
			int val = -1;
			sscanf(argv[1], "%d", &reg);
			sscanf(argv[2], "%x", &val);

			if (reg >= 0 && val >= 0) {
				drv8305_write_reg(reg, val);
				unsigned int res = drv8305_read_reg(reg);
				char bl[9];
				char bh[9];

				utils_byte_to_binary((res >> 8) & 0xFF, bh);
				utils_byte_to_binary(res & 0xFF, bl);

				commands_printf("New reg value 0x%02x: %s %s (0x%04x)\n", reg, bh, bl, res);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires two arguments.\n");
		}
#else
		commands_printf("This hardware does not have a DRV8305.\n");
#endif
	}
	else if (strcmp(argv[0], "foc_openloop") == 0) {
		if (argc == 3) {
			float current = -1.0;
			float erpm = -1.0;
			sscanf(argv[1], "%f", &current);
			sscanf(argv[2], "%f", &erpm);

			if (current >= 0.0 && erpm >= 0.0) {
				mcpwm_foc_set_openloop(current, erpm);
				commands_printf("setting %f amps and %f rpm to motor 1(remember to activate the send alive button! (heart icon on the right toolbar)",(double) current,(double) erpm);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires two arguments.\n");
		}
	}
	else if (strcmp(argv[0], "foc_openloop2") == 0) {
		if (argc == 3) {
			float current = -1.0;
			float erpm = -1.0;
			sscanf(argv[1], "%f", &current);
			sscanf(argv[2], "%f", &erpm);

			if (current >= 0.0 && erpm >= 0.0) {
				mcpwm_foc_set_openloop2(current, erpm);
				commands_printf("setting %f amps and %f rpm to motor 2(remember to activate the send alive button! (heart icon on the right toolbar)",(double) current,(double) erpm);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires two arguments.\n");
		}
	}
	else if (strcmp(argv[0], "foc_openloopdual") == 0) {
		if (argc == 3) {
			float current = -1.0;
			float erpm = -1.0;
			sscanf(argv[1], "%f", &current);
			sscanf(argv[2], "%f", &erpm);

			if (current >= 0.0 && erpm >= 0.0) {
				mcpwm_foc_set_openloop(current, erpm);
				mcpwm_foc_set_openloop2(current, erpm);
				commands_printf("setting %f amps and %f rpm to motor 1 and 2(remember to activate the send alive button! (heart icon on the right toolbar)",(double) current,(double) erpm);
			}
			else {
				commands_printf("Invalid argument(s).\n");
			}
		}
		else {
			commands_printf("This command requires two arguments.\n");
		}
	}

	// The help command
	else if (strcmp(argv[0], "help") == 0) {
		commands_printf("Valid commands are:");
		commands_printf("help");
		commands_printf("  Show this help");

		commands_printf("ping");
		commands_printf("  Print pong here to see if the reply works");

		commands_printf("stop");
		commands_printf("  Stop the motor");

		commands_printf("last_adc_duration");
		commands_printf("  The time the latest ADC interrupt consumed");

		commands_printf("kv");
		commands_printf("  The calculated kv of the motor");

		commands_printf("mem");
		commands_printf("  Show memory usage");

		commands_printf("threads");
		commands_printf("  List all threads");

		commands_printf("fault");
		commands_printf("  Prints the current fault code for motor 1");

		commands_printf("fault2");
		commands_printf("  Prints the current fault code for motor 2");

		commands_printf("faults");
		commands_printf("  Prints all stored fault codes and conditions when they arrived for motor 1");

		commands_printf("faults2");
		commands_printf("  Prints all stored fault codes and conditions when they arrived for motor 2");

		commands_printf("rpm");
		commands_printf("  Prints the current electrical RPM");

		commands_printf("tacho");
		commands_printf("  Prints tachometer value");

		commands_printf("tim");
		commands_printf("  Prints tim1 and tim8 settings");

		commands_printf("volt");
		commands_printf("  Prints different voltages");

		commands_printf("param_detect [current] [min_rpm] [low_duty]");
		commands_printf("  Spin up the motor in COMM_MODE_DELAY and compute its parameters.");
		commands_printf("  This test should be performed without load on the motor.");
		commands_printf("  Example: param_detect 5.0 600 0.06");

		commands_printf("rpm_dep");
		commands_printf("  Prints some rpm-dep values");

		commands_printf("can_devs");
		commands_printf("  Prints all CAN devices seen on the bus the past second");

		commands_printf("foc_encoder_detect [current]");
		commands_printf("  Run the motor at 1Hz on open loop and compute encoder settings");

		commands_printf("measure_res [current]");
		commands_printf("  Lock the motor with a current and calculate its resistance");

		commands_printf("measure_ind [duty]");
		commands_printf("  Send short voltage pulses, measure the current and calculate the motor inductance");

		commands_printf("measure_linkage [current] [duty] [min_rpm] [motor_res]");
		commands_printf("  Run the motor in BLDC delay mode and measure the flux linkage");
		commands_printf("  example measure_linkage 5 0.5 700 0.076");
		commands_printf("  tip: measure the resistance with measure_res first");

		commands_printf("measure_res_ind");
		commands_printf("  Measure the motor resistance and inductance with an incremental adaptive algorithm.");

		commands_printf("measure_linkage_foc [duty]");
		commands_printf("  Run the motor with FOC and measure the flux linkage.");

		commands_printf("foc_state");
		commands_printf("  Print some FOC state variables.");

		commands_printf("hw_status");
		commands_printf("  Print some hardware status information.");

#ifdef HW_HAS_DRV8301
		commands_printf("drv8301_read_reg [reg]");
		commands_printf("  Read a register from the DRV8301 and print it.");

		commands_printf("drv8301_write_reg [reg] [hexvalue]");
		commands_printf("  Write to a DRV8301 register.");

		commands_printf("drv8301_set_oc_adj [value]");
		commands_printf("  Set the DRV8301 OC ADJ register.");

		commands_printf("drv8301_print_faults");
		commands_printf("  Print all current DRV8301 faults.");

		commands_printf("drv8301_reset_faults");
		commands_printf("  Reset all latched DRV8301 faults.");
#endif

#ifdef HW_HAS_DRV8305
		commands_printf("drv8305_read_reg [reg]");
		commands_printf("  Read a register from the DRV8305 and print it.");

		commands_printf("drv8305_write_reg [reg] [hexvalue]");
		commands_printf("  Write to a DRV8305 register.");
#endif

		commands_printf("foc_openloop [current] [erpm]");
		commands_printf("  Create an open loop rotating current vector for motor 1.");

		commands_printf("foc_openloop2 [current] [erpm]");
		commands_printf("  Create an open loop rotating current vector for motor 2.");

		commands_printf("foc_openloopdual [current] [erpm]");
		commands_printf("  Create an open loop rotating current vector for motor 1 and 2.");

		commands_printf("calibrate");
		commands_printf("  Run a quick automatic motor calibration routine.");

		for (int i = 0; i < callback_write; i++) {
			if (callbacks[i].arg_names) {
				commands_printf("%s %s", callbacks[i].command, callbacks[i].arg_names);
			}
			else {
				commands_printf(callbacks[i].command);
			}

			if (callbacks[i].help) {
				commands_printf("  %s", callbacks[i].help);
			}
			else {
				commands_printf("  There is no help available for this command.");
			}
		}

		commands_printf(" ");
	}
	else {
		bool found = false;
		for (int i = 0; i < callback_write; i++) {
			if (strcmp(argv[0], callbacks[i].command) == 0) {
				callbacks[i].cbf(argc, (const char**)argv);
				found = true;
				break;
			}
		}

		if (!found) {
			commands_printf("Invalid command: %s\n"
					"type help to list all available commands\n",
					argv[0]);
		}
	}
}

void terminal_add_fault_data(fault_data *data) {
	fault_vec[fault_vec_write++] = *data;
	if (fault_vec_write >= FAULT_VEC_LEN) {
		fault_vec_write = 0;
	}
}

/**
 * Register a custom command  callback to the terminal. If the command
 * is already registered the old command callback will be replaced.
 *
 * @param command
 * The command name.
 *
 * @param help
 * A help text for the command. Can be NULL.
 *
 * @param arg_names
 * The argument names for the command, e.g. [arg_a] [arg_b]
 * Can be NULL.
 *
 * @param cbf
 * The callback function for the command.
 */
void terminal_register_command_callback(const char* command, const char *help, const char *arg_names, void (*cbf)(int argc, const char **argv)) {

	int callback_num = callback_write;

	for (int i = 0; i < callback_write; i++) {
		// First check the address in case the same callback is registered more than once.
		if (callbacks[i].command == command) {
			callback_num = i;
			break;
		}

		// Check by string comparison.
		if (strcmp(callbacks[i].command, command) == 0) {
			callback_num = i;
			break;
		}
	}

	callbacks[callback_num].command = command;
	callbacks[callback_num].help = help;
	callbacks[callback_num].arg_names = arg_names;
	callbacks[callback_num].cbf = cbf;

	if (callback_num == callback_write) {
		callback_write++;
		if (callback_write >= CALLBACK_LEN) {
			callback_write = 0;
		}
	}
}
