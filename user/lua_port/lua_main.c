/*
 * Lantest OS - Lua 5.4 interpreter: fixed demo, REPL, or script file
 */
#include "crt.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <string.h>

static const char hello_script[] =
    "print('Hello from Lantest OS!')\n"
    "local t = {'hello','lua','from','lantest'}\n"
    "table.insert(t, '!')\n"
    "print(string.upper(table.concat(t, ' ')))\n"
    "print(utf8.char(0x4E2D))\n"
    "print(math.floor(3.9), math.sqrt(9))\n"
    "print(math.sin(0), math.cos(0), math.exp(0), math.log(1))\n"
    "print(2 ^ 10, tostring(3.14), string.format('%.2f', 3.14159))\n"
    "local f = io.open('/hello.txt', 'w')\n"
    "if f then\n"
    "  f:write('Written by Lua in Lantest OS!\\n')\n"
    "  f:close()\n"
    "  print('File written.')\n"
    "end\n"
    "local g = io.open('/hello.txt', 'r')\n"
    "if g then\n"
    "  print(g:read('*a'))\n"
    "  g:close()\n"
    "end\n";

static void open_libs(lua_State *L)
{
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_IOLIBNAME, luaopen_io, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    lua_pop(L, 1);
}

static int run_file(lua_State *L, const char *path)
{
    int rc = luaL_loadfile(L, path);
    if (rc != LUA_OK)
        return rc;
    return lua_pcall(L, 0, LUA_MULTRET, 0);
}

static int run_repl(lua_State *L)
{
    char line[128];
    char code[512];
    size_t len = 0;
    int prompt = 1;

    code[0] = '\0';
    printf("lua> ");

    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (len == 0 &&
            (strcmp(line, "quit\n") == 0 || strcmp(line, "exit\n") == 0))
            break;

        size_t n = strlen(line);
        if (len + n + 1 >= sizeof(code)) {
            fprintf(stderr, "lua: input too long\n");
            len = 0;
            code[0] = '\0';
            printf("lua> ");
            continue;
        }
        memcpy(code + len, line, n + 1);
        len += n;

        if (len == 1 && code[0] == '\n') {
            len = 0;
            code[0] = '\0';
            printf("lua> ");
            continue;
        }

        int rc = luaL_loadstring(L, code);
        if (rc == LUA_OK) {
            int prc = lua_pcall(L, 0, LUA_MULTRET, 0);
            if (prc != LUA_OK) {
                fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            len = 0;
            code[0] = '\0';
            printf("lua> ");
            continue;
        }

        const char *err = lua_tostring(L, -1);
        if (err != NULL && strstr(err, "<eof>") != NULL &&
            len + 1 < sizeof(code)) {
            lua_pop(L, 1);
            printf(">> ");
            continue;
        }

        fprintf(stderr, "lua: %s\n", err != NULL ? err : "syntax error");
        lua_pop(L, 1);
        len = 0;
        code[0] = '\0';
        printf("lua> ");
    }

    (void)prompt;
    printf("\n");
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    lua_State *L;
    int rc = 0;

    (void)envp;

    L = luaL_newstate();
    if (L == NULL) {
        printf("lua: cannot create state\n");
        return 1;
    }

    open_libs(L);

    if (argc == 0) {
        rc = luaL_loadstring(L, hello_script);
        if (rc == LUA_OK)
            rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    } else if (argc == 1) {
        rc = run_repl(L);
    } else {
        rc = run_file(L, argv[1]);
    }

    if (rc != LUA_OK) {
        fprintf(stderr, "lua: error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_close(L);
    printf("lua: OK\n");
    return 0;
}
