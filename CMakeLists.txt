cmake_minimum_required(VERSION 3.20.0)

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ZMK_KSCAN_ADC_JOYSTICK drivers/kscan/kscan_adc_joystick.c)
