ifndef GPI2_HOME
  GPI2_HOME=../../../GPI-2
endif

BIN += simple.bin

CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Wshadow
CFLAGS += -O0 -g
CFLAGS += -std=c99
CFLAGS += -D_BSD_SOURCE
###############################################################################

INCLUDE_DIR += $(GPI2_HOME)/include
INCLUDE_DIR += ../../include
LIBRARY_DIR += $(GPI2_HOME)/lib64
LIBRARY_DIR += ../../lib

LDFLAGS += $(addprefix -L,$(LIBRARY_DIR))

CFLAGS += $(addprefix -I,$(INCLUDE_DIR))

LIB += gpi_cp
LIB += ibverbs
LIB += GPI2-dbg
LIB += m

###############################################################################

default: $(BIN)

%.bin: %.o $(addsuffix .o, $(OBJ)) 
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(addprefix -l, $(LIB))

###############################################################################

.PHONY: clean objclean

objclean:
	rm -f *.o *~

clean: objclean
	rm -f $(BIN)
