# The Vulkan renderer's shader compiler (run_tests.sh): rslc, as
# RendererShaderVk hands it GLSL, over the core's own shaders, the shapes the
# decompiler writes and every graphic pack shader the core ships.
# SANITIZER=address,undefined for the sanitizer run.
LRC := ../../dependencies/libretro-common
SRC := ../../src
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I$(SRC) -I$(LRC)/include -DRSLC_EXTENDED -DRSLC_NO_TEXT
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LIBS := -lm
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -static
   EXE := .exe
endif
C_SRCS := $(LRC)/gfx/shaders/rslc_front.c $(LRC)/gfx/shaders/rslc_ir.c $(LRC)/gfx/shaders/rslc_dxbc.c

shaders_test$(EXE): shaders_test.cpp $(C_SRCS) $(SRC)/Cafe/HW/Latte/Renderer/Vulkan/RslcGlsl.h
	for f in $(C_SRCS); do $(CC) $(FLAGS) -std=gnu99 -c $$f -o $$(basename $$f .c).o || exit 1; done
	$(CXX) $(FLAGS) -std=gnu++20 shaders_test.cpp *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f shaders_test shaders_test.exe *.o
