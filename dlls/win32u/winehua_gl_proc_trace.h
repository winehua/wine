/*
 * WineHua GL proc dispatch 探针 (只诊断, 详见 .c)
 * 开关: WINEHUA_GL_PROC_TRACE=1
 */
#ifndef __WINE_WIN32U_WINEHUA_GL_PROC_TRACE_H
#define __WINE_WIN32U_WINEHUA_GL_PROC_TRACE_H

#ifdef __OHOS__

void winehua_gl_proc_trace_note( const char *name, void *result, const char *source );
void winehua_gl_proc_trace_dump( const char *why );

#else

static inline void winehua_gl_proc_trace_note( const char *name, void *result, const char *source )
{ (void)name; (void)result; (void)source; }
static inline void winehua_gl_proc_trace_dump( const char *why ) { (void)why; }

#endif /* __OHOS__ */

#endif /* __WINE_WIN32U_WINEHUA_GL_PROC_TRACE_H */
