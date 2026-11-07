# The curl_compat test (run_tests.sh); SANITIZER=address,undefined or
# SANITIZER=thread for the sanitizer runs.
LRC := ../../libretro-common
CXX ?= g++
CC ?= gcc
FLAGS := -O1 -g -I../include -I$(LRC)/include -DHAVE_SSL -DHAVE_RETROSSL -DHAVE_CRYPTO -DHAVE_THREADS
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
LRC_SOURCES := $(addprefix $(LRC)/,net/net_http.c net/net_socket.c net/net_compat.c \
	net/net_socket_ssl_retro.c crypto/crypto.c crypto/kdf.c crypto/pk.c crypto/x509.c \
	hash/lrc_hash.c utils/md5.c encodings/encoding_base64.c streams/file_stream.c \
	vfs/vfs_implementation.c file/file_path.c file/file_path_io.c string/rstrtod.c \
	string/stdstring.c encodings/encoding_utf.c features/features_cpu.c compat/compat_strl.c \
	compat/fopen_utf8.c compat/compat_strcasestr.c compat/compat_posix_string.c time/rtime.c \
	lists/string_list.c file/retro_dirent.c rthreads/rthreads.c rthreads/retro_eventcount.c)
LIBS := -lpthread
ifneq (,$(findstring mingw,$(CC)))
   LIBS += -lws2_32 -liphlpapi -lbcrypt -static
   EXE := .exe
endif

curl_compat_test$(EXE): curl_compat_test.cpp ../curl_compat.cpp $(LRC_SOURCES)
	$(CC) $(FLAGS) -std=gnu11 -c $(LRC_SOURCES)
	$(CXX) $(FLAGS) -std=gnu++20 curl_compat_test.cpp ../curl_compat.cpp *.o $(LIBS) -o $@
	rm -f *.o

clean:
	rm -f curl_compat_test curl_compat_test.exe *.o
