USB device driver
=================

This module aims at providing a sample kernel USB driver for
registering a USB device on a linux system.

Usage:
	# make clean && make
	# sudo insmod usb.ko
	# tailf -f /var/log/messages
	# sudo rmmod usb.ko

Sample syslog:

8339 Feb 27 17:41:59 localhost kernel: usbcore: registered new interface driver prathamesh driver
8342 Feb 27 17:42:18 localhost kernel: usbcore: deregistering interface driver prathamesh driver

