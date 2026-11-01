# Host-side test of the Open GoPro packetizer / reassembler (no ESP-IDF needed).
#   make -C components/camera_gopro/hosttest
CC ?= cc
tlv_test: tlv_test.c ../gopro_tlv.c ../include/gopro_tlv.h
	$(CC) -O1 -Wall -I../include tlv_test.c ../gopro_tlv.c -o $@
	./$@
clean:
	rm -f tlv_test
.PHONY: clean
