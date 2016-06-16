ifndef GPI2_HOME
  GPI2_HOME = ../../GPI-2
endif

CFLAGS += -Wall -Wextra
CFLAGS += -O0 -g
CFLAGS += -I$(GPI2_HOME)/include
CFLAGS += -I../include
SRCS += gpi_cp.c

OBJS = $(SRCS:.c=.o)

CPLIB = libgpi_cp.a

cplib: $(OBJS)
	-mkdir ../lib/
	-$(RM) ../lib/$(CPLIB)
	$(AR) crs ../lib/$(CPLIB) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $<  -o $@

clean:
	$(RM) $(OBJS) *~ ../lib/$(CPLIB)

.PHONY: clean
