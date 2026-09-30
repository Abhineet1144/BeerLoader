CC     = gcc
CFLAGS = -O0 -Wall -Wextra -m64 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -mno-sse -mno-sse2 -mno-avx -mno-mmx -Igraphics
LDFLAGS = -lpthread -ldl
TARGET = loader
SRCS = loader.c graphics/d3d11_impl.c
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
