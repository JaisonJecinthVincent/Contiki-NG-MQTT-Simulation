CONTIKI_PROJECT = actuator-subscriber
all: $(CONTIKI_PROJECT)

CONTIKI ?= ../../..

include $(CONTIKI)/Makefile.dir-variables
MODULES += $(CONTIKI_NG_APP_LAYER_DIR)/mqtt
MODULES += $(CONTIKI_NG_LIB_DIR)/json

include $(CONTIKI)/Makefile.include
