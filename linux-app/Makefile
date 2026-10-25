CC = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
CFLAGS = -O2 -Wall -Wextra -std=c11
CXXFLAGS = -O2 -Wall -Wextra -std=c++17 -fno-exceptions -fno-rtti

ROOT := $(abspath ..)
ERPC := $(ROOT)/third_party/erpc
SHIM := $(ROOT)/zephyr/erpc/shim
INCLUDES = -I. -I$(SHIM) -I$(ROOT)/zephyr/include -I$(ROOT)/dsp/include \
	-I$(ERPC)/erpc_c/infra -I$(ERPC)/erpc_c/port \
	-I$(ERPC)/erpc_c/setup -I$(ERPC)/erpc_c/transports

ERPC_SRCS = \
	$(ERPC)/erpc_c/infra/erpc_basic_codec.cpp \
	$(ERPC)/erpc_c/infra/erpc_client_manager.cpp \
	$(ERPC)/erpc_c/infra/erpc_crc16.cpp \
	$(ERPC)/erpc_c/infra/erpc_framed_transport.cpp \
	$(ERPC)/erpc_c/infra/erpc_message_buffer.cpp \
	$(ERPC)/erpc_c/infra/erpc_message_loggers.cpp \
	$(ERPC)/erpc_c/infra/erpc_pre_post_action.cpp \
	$(ERPC)/erpc_c/infra/erpc_utils.cpp \
	$(ERPC)/erpc_c/port/erpc_port_stdlib.cpp \
	$(ERPC)/erpc_c/setup/erpc_client_setup.cpp \
	$(ERPC)/erpc_c/setup/erpc_setup_mbf_dynamic.cpp \
	$(SHIM)/c_ai64_client.cpp \
	$(SHIM)/ai64_client.cpp \
	$(SHIM)/ai64_interface.cpp \
	ai64_erpc_linux.cpp

all: ai64

ai64: ai64.o ai64_fabric.o ai64_pipe.o $(ERPC_SRCS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ ai64.o ai64_fabric.o ai64_pipe.o $(ERPC_SRCS) -lm

ai64.o: ai64.c ai64_erpc_linux.h $(ROOT)/zephyr/include/ai64_shm.h $(ROOT)/dsp/include/ai64_dsp_job.h
	$(CC) $(CFLAGS) -I. -I$(ROOT)/zephyr/include -I$(ROOT)/dsp/include -c -o $@ ai64.c

ai64_fabric.o: ai64_fabric.c $(ROOT)/zephyr/include/ai64_fabric.h
	$(CC) $(CFLAGS) -I$(ROOT)/zephyr/include -c -o $@ ai64_fabric.c

ai64_pipe.o: ai64_pipe.c $(ROOT)/zephyr/include/ai64_pipe.h $(ROOT)/zephyr/include/ai64_shm.h
	$(CC) $(CFLAGS) -I$(ROOT)/zephyr/include -c -o $@ ai64_pipe.c

clean:
	rm -f ai64 ai64.o ai64_fabric.o ai64_pipe.o
