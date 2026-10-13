# Host-side test of the RDS text path. Si4713.cpp logs through libfpp, so build on an FPP box:
#   make -C tests SRCDIR=/opt/fpp/src && ./tests/rds_text_test
SRCDIR ?= /opt/fpp/src
CXX ?= g++
CC ?= gcc
rds_text_test: rds_text_test.cpp ../src/Si4713.cpp ../src/Si4713.h ../src/RDSCharset.h ../src/bitstream.c
	$(CC) -c -o bitstream.o ../src/bitstream.c
	$(CXX) -std=gnu++23 -DNOPCH -I$(SRCDIR) -I../src -Wall -o $@ rds_text_test.cpp ../src/Si4713.cpp bitstream.o -L$(SRCDIR) -lfpp -Wl,-rpath,$(SRCDIR)
clean:
	rm -f rds_text_test bitstream.o
