#define main beer_unused_main
#include "../loader.c"
#undef main
#include <assert.h>

int main(int argc, char **argv)
{
    alarm(25);
    if (argc > 1 && !strncmp(argv[1], "--beer-cmd=", 11)) {
        if (!beer_child_startup()) return 8;
        const char *path = strrchr(argv[1], ' ');
        assert(path && !getenv("_BEER_CHILD_CONTROL"));
        int fd = open(path + 1, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        assert(write(fd, "G", 1) == 1);
        close(fd);
        (void)poll(NULL, 0, 200);
        return 27;
    }
    char path[] = "/tmp/beer-child-wrapper-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0); close(fd); unlink(path);
    u8 startup[104] = {0}, info[24] = {0};
    *(u32 *)startup = 104;
    char command[256];
    snprintf(command, sizeof(command), "cmd.exe fixture %s", path);
    assert(!beer_CreateProcessA("cmd.exe", command, NULL, NULL, 0, 4, (void *)1, NULL, startup, info));
    assert(g_last_error == 50);
    assert(!beer_CreateProcessA("cmd.exe", command, NULL, NULL, 0, 4, NULL, NULL, startup, NULL));
    assert(g_last_error == 87);
    assert(beer_CreateProcessA("cmd.exe", command, NULL, NULL, 0, 4, NULL, NULL, startup, info));
    u64 process, thread;
    memcpy(&process, info, 8); memcpy(&thread, info + 8, 8);
    assert(beer_primary_namespace(thread));
    u8 context[1232]; memset(context, 0x5a, sizeof(context));
    assert(!beer_GetThreadContext(thread, context) && g_last_error == 50);
    assert(context[0x48] == 0x5a);
    assert(!stub_SetThreadContext(thread, context) && g_last_error == 50);
    assert(impl_WaitForSingleObject(thread, 30) == 0x102);
    assert(impl_WaitForSingleObject(process, 0) == 0x102);
    assert(access(path, F_OK) == -1);
    u64 handles[2] = {thread, process};
    assert(impl_WaitForMultipleObjects(2, handles, 1, 0) == 0x102);
    assert(impl_ResumeThread(thread) == 1);
    assert(impl_WaitForMultipleObjects(2, handles, 1, 5000) == 0);
    u32 code;
    assert(impl_GetExitCodeThread(thread, &code) && code == 27);
    assert(beer_GetExitCodeProcess(process, &code) && code == 27);
    assert(access(path, F_OK) == 0); unlink(path);
    assert(impl_CloseHandle(process));
    assert(impl_GetExitCodeThread(thread, &code) && code == 27);
    assert(impl_CloseHandle(thread));
    assert(!impl_CloseHandle(thread) && g_last_error == 6);
    assert(impl_ResumeThread(thread) == 0xffffffffu && g_last_error == 6);
    assert(!impl_TerminateProcess(0x1234, 1) && g_last_error == 6);
    assert(beer_CreateProcessA("cmd.exe", command, NULL, NULL, 0, 4, NULL, NULL, startup, info));
    memcpy(&process, info, 8); memcpy(&thread, info + 8, 8);
    assert(impl_TerminateThread(thread, 0xf2345678));
    assert(impl_WaitForSingleObject(thread, 5000) == 0);
    assert(impl_GetExitCodeThread(thread, &code) && code == 0xf2345678);
    assert(access(path, F_OK) == -1);
    assert(impl_CloseHandle(thread) && impl_CloseHandle(process));
    puts("child Win32 loader wiring: PASS");
    return 0;
}