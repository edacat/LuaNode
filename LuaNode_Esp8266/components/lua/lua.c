/*
** $Id: lua.c,v 1.160.1.2 2007/12/28 15:32:23 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lua_c

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

#include "esp_common.h"
#include "uart.h"
#include "lptree.h"

char line_buffer[LUA_MAXINPUT];

static lua_State *lua_crtstate = NULL;

static lua_State *globalL = NULL;

lua_Load gLoad;

static const char *progname = LUA_PROGNAME;



static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "interrupted!");
}


static void laction (int i) {
  signal(i, SIG_DFL); /* if another SIGINT happens before lstop,
                              terminate process (default action) */
  lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


static void print_usage (void) {
  fprintf(stderr,
  "usage: %s [options] [script [args]].\n"
  "Available options are:\n"
  "  -e stat  execute string " LUA_QL("stat") "\n"
  "  -l name  require library " LUA_QL("name") "\n"
  "  -m limit set memory limit. (units are in Kbytes)\n"
  "  -i       enter interactive mode after executing " LUA_QL("script") "\n"
  "  -v       show version information\n"
  "  --       stop handling options\n"
  "  -        execute stdin and stop handling options\n"
  ,
  progname);
  fflush(stderr);
}


static void l_message (const char *pname, const char *msg) {
  //if (pname) fprintf(stderr, "%s: ", pname);
  //fprintf(stderr, "%s\n", msg);
  //fflush(stderr);
}


static int report (lua_State *L, int status) {
  if (status && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) msg = "(error object is not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);
  }
  return status;
}


static int traceback (lua_State *L) {
  if (!lua_isstring(L, 1))  /* 'message' not a string? */
    return 1;  /* keep it intact */
  lua_getfield(L, LUA_GLOBALSINDEX, "debug");
  if (!lua_istable(L, -1) && !lua_isrotable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_getfield(L, -1, "traceback");
  if (!lua_isfunction(L, -1) && !lua_islightfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushvalue(L, 1);  /* pass error message */
  lua_pushinteger(L, 2);  /* skip this function and traceback */
  lua_call(L, 2, 1);  /* call debug.traceback */
  return 1;
}


static int docall (lua_State *L, int narg, int clear) {
#if 0
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, traceback);  /* push traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  signal(SIGINT, laction);
  status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
  signal(SIGINT, SIG_DFL);
  lua_remove(L, base);  /* remove traceback function */
  /* force a complete garbage collection in case of errors */
  if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
#endif
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
  return status;
}


static void print_version (void) {
  l_message(NULL, LUA_RELEASE "  " LUA_COPYRIGHT);
}


static int getargs (lua_State *L, char **argv, int n) {
  int narg;
  int i;
  int argc = 0;
  while (argv[argc]) argc++;  /* count total number of arguments */
  narg = argc - (n + 1);  /* number of arguments to the script */
  luaL_checkstack(L, narg + 3, "too many arguments to script");
  for (i=n+1; i < argc; i++)
    lua_pushstring(L, argv[i]);
  lua_createtable(L, narg, n + 1);
  for (i=0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - n);
  }
  return narg;
}


static int dofsfile (lua_State *L, const char *name) {
  int status = luaL_loadfsfile(L, name) || docall(L, 0, 1);
  return report(L, status);
}


static int dostring (lua_State *L, const char *s, const char *name) {
  int status = luaL_loadbuffer(L, s, strlen(s), name) || docall(L, 0, 1);
  return report(L, status);
}


static int dolibrary (lua_State *L, const char *name) {
  lua_getglobal(L, "require");
  lua_pushstring(L, name);
  return report(L, docall(L, 1, 1));
}


static const char *get_prompt (lua_State *L, int firstline) {
  const char *p;
  lua_getfield(L, LUA_GLOBALSINDEX, firstline ? "_PROMPT" : "_PROMPT2");
  p = lua_tostring(L, -1);
  if (p == NULL) p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
  lua_pop(L, 1);  /* remove global */
  return p;
}


static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    const char *tp = msg + lmsg - (sizeof(LUA_QL("<eof>")) - 1);
    if (strstr(msg, LUA_QL("<eof>")) == tp) {
      lua_pop(L, 1);
      return 1;
    }
  }
  return 0;  /* else... */
}


static int pushline (lua_State *L, int firstline) {
  char buffer[LUA_MAXINPUT];
  char *b = buffer;
  size_t l;
  const char *prmt = get_prompt(L, firstline);
  if (lua_readline(L, b, prmt) == 0)
    return 0;  /* no input */
  l = strlen(b);
  if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
    b[l-1] = '\0';  /* remove it */
  if (firstline && b[0] == '=')  /* first line starts with `=' ? */
    lua_pushfstring(L, "return %s", b+1);  /* change it to `return' */
  else
    lua_pushstring(L, b);
  lua_freeline(L, b);
  return 1;
}


static int loadline (lua_State *L) {
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;  /* no input */
  for (;;) {  /* repeat until gets a complete line */
    status = luaL_loadbuffer(L, lua_tostring(L, 1), lua_strlen(L, 1), "=stdin");
    if (!incomplete(L, status)) break;  /* cannot try to add lines? */
    if (!pushline(L, 0))  /* no more input? */
      return -1;
    lua_pushliteral(L, "\n");  /* add a new line... */
    lua_insert(L, -2);  /* ...between the two lines */
    lua_concat(L, 3);  /* join them */
  }
  lua_saveline(L, 1);
  lua_remove(L, 1);  /* remove line */
  return status;
}


static void dotty (lua_State *L) {
  int status;
  const char *oldprogname = progname;
  progname = NULL;
  while ((status = loadline(L)) != -1) {
    if (status == 0) status = docall(L, 0, 0);
    report(L, status);
    if (status == 0 && lua_gettop(L) > 0) {  /* any result to print? */
      lua_getglobal(L, "print");
      lua_insert(L, 1);
      if (lua_pcall(L, lua_gettop(L)-1, 0, 0) != 0)
        l_message(progname, lua_pushfstring(L,
                               "error calling " LUA_QL("print") " (%s)",
                               lua_tostring(L, -1)));
    }
  }
  lua_settop(L, 0);  /* clear stack */
  fputs("\n", stdout);
  fflush(stdout);
  progname = oldprogname;
}


static int handle_script (lua_State *L, char **argv, int n) {
  int status;
  const char *fname;
  int narg = getargs(L, argv, n);  /* collect arguments */
  lua_setglobal(L, "arg");
  fname = argv[n];
  if (strcmp(fname, "-") == 0 && strcmp(argv[n-1], "--") != 0) 
    fname = NULL;  /* stdin */
  status = luaL_loadfile(L, fname);
  lua_insert(L, -(narg+1));
  if (status == 0)
    status = docall(L, narg, 0);
  else
    lua_pop(L, narg);      
  return report(L, status);
}


/* check that argument has no extra characters at the end */
#define notail(x)	{if ((x)[2] != '\0') return -1;}


static int collectargs (char **argv, int *pi, int *pv, int *pe) {
  int i;
  for (i = 1; argv[i] != NULL; i++) {
    if (argv[i][0] != '-')  /* not an option? */
        return i;
    switch (argv[i][1]) {  /* option */
      case '-':
        notail(argv[i]);
        return (argv[i+1] != NULL ? i+1 : 0);
      case '\0':
        return i;
      case 'i':
        notail(argv[i]);
        *pi = 1;  /* go through */
      case 'v':
        notail(argv[i]);
        *pv = 1;
        break;
      case 'e':
        *pe = 1;  /* go through */
      case 'm':   /* go through */
      case 'l':
        if (argv[i][2] == '\0') {
          i++;
          if (argv[i] == NULL) return -1;
        }
        break;
      default: return -1;  /* invalid option */
    }
  }
  return 0;
}

static int runargs (lua_State *L, char **argv, int n) {
  int i;
  for (i = 1; i < n; i++) {
    if (argv[i] == NULL) continue;
    lua_assert(argv[i][0] == '-');
    switch (argv[i][1]) {  /* option */
      case 'e': {
        const char *chunk = argv[i] + 2;
        if (*chunk == '\0') chunk = argv[++i];
        lua_assert(chunk != NULL);
        if (dostring(L, chunk, "=(command line)") != 0)
          return 1;
        break;
      }
      case 'm': {
        const char *limit = argv[i] + 2;
        int memlimit=0;
        if (*limit == '\0') limit = argv[++i];
        lua_assert(limit != NULL);
        memlimit = atoi(limit);
        lua_gc(L, LUA_GCSETMEMLIMIT, memlimit);
        break;
      }
      case 'l': {
        const char *filename = argv[i] + 2;
        if (*filename == '\0') filename = argv[++i];
        lua_assert(filename != NULL);
        if (dolibrary(L, filename))
          return 1;  /* stop if file fails */
        break;
      }
      default: break;
    }
  }
  return 0;
}

static int handle_luainit (lua_State *L) {
  return dofsfile(L, "init.lua");
}

int do_luainit (void) {
	lua_State *L = gLoad.L;
	return dofsfile(L, "init.lua");
}

struct Smain {
  int argc;
  char **argv;
  int status;
};


static int pmain (lua_State *L) {
  struct Smain *s = (struct Smain *)lua_touserdata(L, 1);
  char **argv = s->argv;
  int script;
  int has_i = 0, has_v = 0, has_e = 0;
  globalL = L;
  if (argv[0] && argv[0][0]) progname = argv[0];
  lua_gc(L, LUA_GCSTOP, 0);  /* stop collector during initialization */
  luaL_openlibs(L);  /* open libraries */
  lua_gc(L, LUA_GCRESTART, 0);
  s->status = handle_luainit(L);
  if (s->status != 0) return 0;
  script = collectargs(argv, &has_i, &has_v, &has_e);
  if (script < 0) {  /* invalid args? */
    print_usage();
    s->status = 1;
    return 0;
  }
  if (has_v) print_version();
  s->status = runargs(L, argv, (script > 0) ? script : s->argc);
  if (s->status != 0) return 0;
  if (script)
    s->status = handle_script(L, argv, script);
  if (s->status != 0) return 0;
  if (has_i)
    dotty(L);
  else if (script == 0 && !has_e && !has_v) {
    if (lua_stdin_is_tty()) {
      print_version();
      dotty(L);
    }
    else dofile(L, NULL);  /* executes stdin as a file */
  }
  return 0;
}

static void dojob(lua_Load *load);
static bool readline(lua_Load *load);

int lua_main (int argc, char **argv) {
  int status;
  struct Smain s;
  
  lua_State *L = luaL_newstate();
  if (L == NULL) {
    printf("lua failed\n");
    return 1;
  }
  printf("lua create ok!\n");
  
  //luaopen_base(L);
  luaL_openlibs(L);
  luaopen_lpeg(L);

  /*s.argc = argc;
  s.argv = argv;
  status = lua_cpcall(L, &pmain, &s);
  report(L, status);*/

  gLoad.L = L;
  gLoad.firstline = 1;
  gLoad.done = 0;
  gLoad.line = line_buffer;
  gLoad.len = LUA_MAXINPUT;
  gLoad.line_position = 0;
  gLoad.prmt = get_prompt(L, 1);

  dojob(&gLoad);

  //status = handle_luainit(L);

  //const char *buff = "local str=222; print(str)";
  //luaL_dostring(L, buff);

  /*const char *buff = "tbl = 25";
  luaL_dostring(L, buff);
  lua_getglobal(L, "tbl");
  int tbl = lua_tointeger(L, -1);
  printf("tbl=%d\n", tbl);*/

  /*const char *buff2 = "function lua_add(a,b) return add(a,b); end";
  lua_register(L, "add", add);
  luaL_dostring(L, buff2);
  lua_getglobal(L, "lua_add");
  lua_pushinteger(L, 6);
  lua_pushinteger(L, 5);
  lua_pcall(L, 2, 1, 0);
  int res = lua_tointeger(L, -1);
  printf("result=%d\n", res);*/

  return 0;
}

void lua_test(void) {
  lua_State *L = gLoad.L;
  const char *buff = "local str=222; print(str)";
  luaL_dostring(L, buff);
}

void lua_handle_input (bool force)
{
  if (force || readline (&gLoad)) {
    dojob (&gLoad);
  }
}

bool uart_getc(char *c){
	RcvMsgBuff *pRxBuff = &rcvMsgBuff;
    if(pRxBuff->pWritePos == pRxBuff->pReadPos){   // empty
      return false;
    }
    // ETS_UART_INTR_DISABLE();
    ETS_INTR_LOCK();
    *c = (char)*(pRxBuff->pReadPos);
    if (pRxBuff->pReadPos == (pRxBuff->pRcvMsgBuff + RX_BUFF_SIZE)) {
        pRxBuff->pReadPos = pRxBuff->pRcvMsgBuff ; 
    } else {
        pRxBuff->pReadPos++;
    }
    // ETS_UART_INTR_ENABLE();
    ETS_INTR_UNLOCK();
    return true;
}

static void dojob(lua_Load *load) {
  size_t l, rs;
  int status;
  char *b = load->line;
  lua_State *L = load->L;
  
  const char *oldprogname = progname;
  progname = NULL;
  
  do{
    if(load->done == 1){
      l = strlen(b);
      if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
        b[l-1] = '\0';  /* remove it */
	  status = luaL_dostring(L, b);
	  
	  if (status) {
		printf(lua_tostring(L, -1));
		lua_pop(L, 1);
	  }

	  load->firstline = 0;
      load->prmt = get_prompt(L, 1);
    }
  }while(0);
  
  progname = oldprogname;

//  char *b = load->line;
//  lua_State *L = load->L;
//  luaL_dostring(L, b);

  load->done = 0;
  load->line_position = 0;
  memset(load->line, 0, load->len);
  printf(load->prmt);
}

static char last_nl_char = '\0';
static bool readline(lua_Load *load) {
  bool need_dojob = false;
  char ch;

  while (uart_getc(&ch)) {
    char tmp_last_nl_char = last_nl_char;
    // reset marker, will be finally set below when newline is processed
    last_nl_char = '\0';
    
    /* handle CR & LF characters
      filters second char of LF&CR (\n\r) or CR&LF (\r\n) sequences */
    if ((ch == '\r' && tmp_last_nl_char == '\n') || // \n\r sequence -> skip \r
          (ch == '\n' && tmp_last_nl_char == '\r'))   // \r\n sequence -> skip \n
    {
      continue;
    }
      
    if (ch == 0x7f || ch == 0x08) {
      if (load->line_position > 0) {
        uart_tx_one_char(UART0, 0x08);
        uart_tx_one_char(UART0, ' ');
        uart_tx_one_char(UART0, 0x08);
        load->line_position--;
      }
      line_buffer[load->line_position] = 0;
      continue;
    }
    
    /* end of line */
    if (ch == '\r' || ch == '\n') {
      last_nl_char = ch;
      line_buffer[load->line_position] = 0;
	  printf("\n");
      if (load->line_position == 0){
        /* Get a empty line, then go to get a new line */
		printf(load->prmt);
      } else {
		load->done = 1;
        need_dojob = true;
      }
      continue;
    }
    
	uart_tx_one_char(UART0, ch);

    /* it's a large line, discard it */
    if ( load->line_position + 1 >= LUA_MAXINPUT ){
      load->line_position = 0;
    }
    
    line_buffer[load->line_position] = ch;
    load->line_position++;
  }

  return need_dojob;
}


void donejob(lua_Load *load){
  lua_close(load->L);
}

