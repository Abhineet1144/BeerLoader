CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx
LDFLAGS = -lpthread -ldl
TARGET = loader

$(TARGET): loader.c
	$(CC) $(CFLAGS) -o $(TARGET) loader.c $(LDFLAGS)

clean:
	rm -f $(TARGET)
