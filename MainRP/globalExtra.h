#define REFIT_DEBUG (0)

VOID
DebugLog (
  IN        INTN  DebugMode,
  IN  CONST CHAR8 *FormatString, ...
);

#if REFIT_DEBUG == 0
  #define MsgLog(...)
#else
  #define MsgLog(...)  DebugLog(REFIT_DEBUG, __VA_ARGS__)
#endif
