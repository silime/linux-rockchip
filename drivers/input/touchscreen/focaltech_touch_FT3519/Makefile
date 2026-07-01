# Makefile for the focaltech touchscreen drivers.



obj-$(CONFIG_TOUCHSCREEN_FTS_FT3519) += focaltech_tp_ft3519.o
focaltech_tp_ft3519-y := focaltech_core.o \
		focaltech_ex_fun.o \
		focaltech_ex_mode.o \
		focaltech_gesture.o \
		focaltech_esdcheck.o \
		focaltech_point_report_check.o \
		focaltech_proximity.o



focaltech_tp_ft3519-y += focaltech_i2c.o






focaltech_tp_ft3519-y += focaltech_flash.o
focaltech_tp_ft3519-y += focaltech_flash/focaltech_upgrade_ft3519t.o
