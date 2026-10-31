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

# A file per subcommand (README.md), and what they share.
SRCS = ai64.c util.c cores.c linux_window.c erpc_window.c rtu0_window.c pwm.c sched.c \
	pads.c trace.c bench.c shutdown.c dsp.c mma.c fabric.c pipe.c web.c
OBJS = $(SRCS:.c=.o) web_page.o
HDRS = ai64.h ai64_erpc_linux.h $(wildcard $(ROOT)/zephyr/include/*.h) $(ROOT)/dsp/include/ai64_dsp_job.h

ai64: $(OBJS) $(ERPC_SRCS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ $(OBJS) $(ERPC_SRCS) -lm

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(ROOT)/zephyr/include -I$(ROOT)/dsp/include -c -o $@ $<

# The page, built into the program (.incbin, relative to this directory).
web_page.o: web_page.S web/index.html
	$(CC) -c -o $@ web_page.S

clean:
	rm -f ai64 $(OBJS)
