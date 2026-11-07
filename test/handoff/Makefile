# The lock-free handoffs the core's threads use (run_tests.sh): the
# recompiler's work queue, the GPU thread's wake, the audio ring.
# SANITIZER=thread or SANITIZER=address,undefined for the sanitizer runs.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DHAVE_THREADS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
C_SRCS := $(LRC)/queues/retro_spsc.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c
CXX_SRCS := handoff_test.cpp \
	$(SRC)/Cafe/HW/Espresso/Recompiler/PPCRecompilerWorkQueue.cpp \
	$(SRC)/Cafe/HW/Latte/Core/LatteGpuWake.cpp

handoff_test$(EXE): $(CXX_SRCS) $(C_SRCS) $(SRC)/libretro/LibretroAudioRing.h
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/queues/retro_spsc.c -o retro_spsc.o
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/rthreads/retro_eventcount.c -o retro_eventcount.o
	$(CC) $(FLAGS) -std=gnu99 -c $(LRC)/rthreads/rthreads.c -o rthreads.o
	$(CXX) $(FLAGS) -std=gnu++20 $(CXX_SRCS) retro_spsc.o retro_eventcount.o rthreads.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f handoff_test handoff_test.exe *.o
