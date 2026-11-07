# The GPU cothread test (run_tests.sh). SANITIZER=address,undefined for the
# sanitizer run; ThreadSanitizer does not follow stack switches, so not that.
# The flags libco is built with are the core's (Makefile.common): LIBCO_MP,
# and Windows' own fibers under MinGW.
LRC := ../../dependencies/libretro-common
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I../../src -I$(LRC)/include -DENABLE_OPENGL -DLIBCO_MP
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBCO := $(LRC)/libco/libco.c
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBCO := $(LRC)/libco/fiber.c
   LIBS += -static
   EXE := .exe
endif

gpu_cothread_test$(EXE): gpu_cothread_test.cpp ../../src/Cafe/HW/Latte/Core/LatteGpuCothread.cpp $(LIBCO)
	$(CC) $(FLAGS) -std=gnu11 -c $(LIBCO) -o libco.o
	$(CXX) $(FLAGS) -std=gnu++20 gpu_cothread_test.cpp ../../src/Cafe/HW/Latte/Core/LatteGpuCothread.cpp libco.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f gpu_cothread_test gpu_cothread_test.exe *.o
