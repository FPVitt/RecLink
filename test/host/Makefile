# Host-side unit tests (no ESP-IDF needed). Pure logic only: CRCs, MSP codec/parser, DJI frames, the
# record state machine, switch decode, BLE status classification, the GoPro packetizer, and the
# Remote ID receiver (matchers + fuzz, decode/table, geometry/levels, %rp render, cfg v7, heading latch).
#   make -C test/host          # build with ASan+UBSan and run everything
#   make -C test/host HOST_LOG=1   # also print the firmware's log lines
CC      ?= cc
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter $(SAN) $(if $(HOST_LOG),-DHOST_LOG=1)
C       := ../../components
INC     := -Istubs -I. -I$(C)/common/include -I$(C)/msp/include -I$(C)/camera/include -I$(C)/cfg/include \
           -I$(C)/control/include -I$(C)/osd/include -I$(C)/camera_dji/include -I$(C)/camera_gopro/include \
           -I$(C)/rid/include -I$(C)/rid/opendroneid

TESTS := test_crc test_msp test_dji test_record test_ble_status test_gopro_tlv test_gopro_presets test_rid test_cfg_rid test_hdg test_osd_rp test_osd_protune test_cfg_cam

all: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "ALL HOST TESTS PASSED"

test_crc: test_crc.c $(C)/common/crc.c $(C)/camera_dji/dji_crc.c
test_msp: test_msp.c $(C)/msp/msp_codec.c $(C)/msp/msp_parse.c $(C)/common/crc.c
test_dji: test_dji.c $(C)/camera_dji/dji_duml.c $(C)/camera_dji/dji_crc.c
test_record: test_record.c $(C)/control/ctrl_record.c $(C)/control/ctrl_switch.c
test_ble_status: test_ble_status.c
test_gopro_tlv: test_gopro_tlv.c $(C)/camera_gopro/gopro_tlv.c
test_gopro_presets: test_gopro_presets.c $(C)/camera_gopro/gopro_presets.c
test_rid: test_rid.c $(C)/rid/rid_match.c $(C)/rid/rid_table.c $(C)/rid/rid_geo.c $(C)/rid/rid_render.c \
          $(C)/rid/rid_json.c $(C)/rid/opendroneid/opendroneid.c
test_cfg_rid: test_cfg_rid.c $(C)/cfg/cfg.c $(C)/cfg/cfg_rid.c $(C)/cfg/cfg_migrate.c $(C)/cfg/cfg_defaults.c \
              $(C)/cfg/cfg_profile.c $(C)/common/crc.c
test_cfg_cam: test_cfg_cam.c $(C)/cfg/cfg.c $(C)/cfg/cfg_rid.c $(C)/cfg/cfg_migrate.c $(C)/cfg/cfg_defaults.c \
              $(C)/cfg/cfg_profile.c $(C)/common/crc.c
test_hdg: test_hdg.c $(C)/msp/hdg_align.c
test_osd_rp: test_osd_rp.c $(C)/osd/osd_format.c $(C)/rid/rid_render.c $(C)/rid/rid_geo.c
test_osd_protune: test_osd_protune.c $(C)/osd/osd_format.c $(C)/rid/rid_render.c $(C)/rid/rid_geo.c

$(TESTS):
	$(CC) $(CFLAGS) $(INC) $^ -o $@ $(LDLIBS)

test_rid: CFLAGS += -DODID_DISABLE_PRINTF
LDLIBS := -lm

clean:
	rm -rf $(TESTS) *.dSYM
.PHONY: all clean
