# Guest C++ Runtime `libSystem.B.dylib` Import Audit (S4)

**Initiative:** Guest-Dylib Initiative — Wave 1 (`S4`)  
**Tool:** `tools/guestlibs/import_audit.py`  
**Refs:** `#311`, `#330`

## 1. Executive Summary

Before enabling guest C++ runtime dynamic libraries (`libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib`) behind the per-app `guestLibs` switch, every external C/POSIX/Mach symbol those libraries import from `/usr/lib/libSystem.B.dylib` must be cross-checked against HydrogenVM's host-side `src/runtime/libc/` registrations.

Across all four ARMv7 C++ runtime libraries, there are **169 unique `libSystem.B.dylib` imports**:
- **`present`**: **105** symbols (implemented in `src/runtime/libc/` with real or honest-approx behaviour)
- **`stubbed-dishonestly`**: **2** symbols (registered in `src/runtime/libc/`, but returning a fixed `"C"` locale pointer or no-op success without full state)
- **`unverified`**: **0** symbols (registered, but no body was resolved; not counted as implemented)
- **`missing`**: **62** symbols (not registered in HydrogenVM's symbol table; tracked in `#330`)

Unresolved HLE bodies remain unverified even if support metadata says Real. This source audit is not a runtime API-conformance result.

### Per-Library Summary

| Guest Dylib | Target Family | Total `libSystem` Imports | `present` | `stubbed-dishonestly` | `unverified` | `missing` |
|---|---|---:|---:|---:|---:|---:|
| `libgcc_s.1.dylib` | Target 1 (iOS 3–5 GCC unwinder) | 14 | 14 | 0 | 0 | 0 |
| `libstdc++.6.dylib` | Target 1 (iOS 3–5 GNU libstdc++) | 77 | 72 | 1 | 0 | 4 |
| `libc++abi.dylib` | Target 2 (iOS 5+ LLVM libc++abi) | 35 | 33 | 0 | 0 | 2 |
| `libc++.1.dylib` | Target 2 (iOS 5+ LLVM libc++) | 159 | 97 | 2 | 0 | 60 |

---

## 2. Key Findings & Actionable Gaps

### 2.1 `libgcc_s.1.dylib` Coverage (`14 / 14 present`, `0 stubbed-dishonestly`, `0 missing`)

Every `libSystem.B.dylib` import required by `libgcc_s.1.dylib` is classified in the symbol table below.
- **Stubbed dishonestly**: none.
- **Unverified**: none.

### 2.2 `libc++abi.dylib` Coverage (`33 / 35 present`, `0 stubbed-dishonestly`, `2 missing`)

- **Missing (2)**: `_aligned_alloc`, `_dyld_register_func_for_add_image`.
- **Stubbed dishonestly**: none.
- **Unverified**: none.

### 2.3 `libstdc++.6.dylib` Coverage (`72 / 77 present`, `1 stubbed-dishonestly`, `4 missing`)

- **Unverified**: none.
- **Missing (4)**: `_btowc`, `_mbrtowc`, `_strxfrm`, `_wcrtomb`.
- **Stubbed dishonestly (1)**: `_setlocale`.

### 2.4 `libc++.1.dylib` Missing Symbol Clusters (`97 / 159 present`, `2 stubbed-dishonestly`, `60 missing`)

- **Unverified**: none.
- **Stubbed dishonestly (2)**: `_pthread_mutexattr_destroy`, `_setlocale`.
- **Extended Locale (`<xlocale.h>`) Lifecycle & `*_l` Functions (42 symbols)**:
  - Locale lifecycle (4): `_duplocale`, `_freelocale`, `_newlocale`, `_uselocale`.
  - Character classification, case mapping & `localeconv_l` (26): `_isalnum_l`, `_isalpha_l`, `_isblank_l`, `_iscntrl_l`, `_isdigit_l`, `_isgraph_l`, `_islower_l`, `_isprint_l`, `_ispunct_l`, `_isspace_l`, `_isupper_l`, `_iswalpha_l`, `_iswblank_l`, `_iswcntrl_l`, `_iswdigit_l`, `_iswgraph_l`, `_iswlower_l`, `_iswprint_l`, `_iswpunct_l`, `_iswspace_l`, `_iswupper_l`, `_iswxdigit_l`, `_isxdigit_l`, `_localeconv_l`, `_towlower_l`, `_towupper_l`.
  - Numeric/string formatting & collation with explicit `locale_t` (12): `_snprintf_l`, `_sscanf_l`, `_strcoll_l`, `_strftime_l`, `_strtod_l`, `_strtof_l`, `_strtold_l`, `_strtoll_l`, `_strtoull_l`, `_strxfrm_l`, `_wcscoll_l`, `_wcsxfrm_l`.
- **Multibyte / Wide-Character Conversion & Collation (`<wchar.h>` / `<string.h>`) Functions (13 symbols)**: `_btowc`, `_mbrlen`, `_mbrtowc`, `_mbsnrtowcs`, `_mbsrtowcs`, `_mbtowc`, `_strxfrm`, `_wcrtomb`, `_wcscoll`, `_wcsnrtombs`, `_wcsrtombs`, `_wcsxfrm`, `_wctob`.
- **Long-Double String Parsers (2 symbols)**: `_strtold`, `_wcstold`.
- **POSIX Message Catalogs (3 symbols)**: `_catclose`, `_catgets`, `_catopen`.
- **Other Unclassified Symbols (0 symbols)**: none.

---

## 3. Complete Symbol-by-Symbol Audit Table

| Symbol | Imported By | Status | HydrogenVM Source | Notes |
|---|---|---|---|---|
| `___cxa_atexit` | `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:416` | Handler `p_cxa_atexit` |
| `___error` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1086` | Handler `s_error` |
| `___mb_cur_max` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:438` | Guest data symbol |
| `___stderrp` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:539` | Guest global data cell |
| `___stdinp` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:539` | Guest global data cell |
| `___stdoutp` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:539` | Guest global data cell |
| `_abort` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:414` | Handler `p_abort` |
| `_aligned_alloc` | `libc++abi.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_asprintf` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:556` | Handler `f_asprintf` |
| `_btowc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_calloc` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:337` | Handler `m_calloc` |
| `_catclose` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_catgets` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_catopen` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_clock_gettime` | `libc++.1.dylib` | `present` | `src/runtime/libc/Time.cpp:144` | Handler `t_clock_gettime` |
| `_close` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1146` | Handler `f_close` |
| `_dladdr` | `libgcc_s.1.dylib`, `libc++abi.dylib` | `present` | `src/runtime/libc/System.cpp:1142` | Handler `d_dladdr` |
| `_duplocale` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_dyld_register_func_for_add_image` | `libc++abi.dylib` | `missing` | `src/runtime/libc/System.cpp:1133` | Registered only under `__dyld_register_func_for_add_image` (2 leading underscores); single-underscore alias `_dyld_register_func_for_add_image` is missing (S5 scope) |
| `_fflush` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:587` | Handler `f_fflush` |
| `_fgetc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:573` | Handler `f_fgetc` |
| `_fprintf` | `libc++abi.dylib` | `present` | `src/runtime/libc/Stdio.cpp:546` | Handler `f_fprintf` |
| `_fputc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:561` | Handler `f_fputc` |
| `_fputs` | `libc++abi.dylib` | `present` | `src/runtime/libc/Stdio.cpp:560` | Handler `f_fputs` |
| `_fread` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:572` | Handler `f_fread` |
| `_free` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:340` | Handler `m_free` |
| `_freelocale` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_fseek` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:579` | Handler `f_fseek` |
| `_fseeko` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:580` | Handler `f_fseeko` |
| `_ftell` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:581` | Handler `f_ftell` |
| `_ftello` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:582` | Handler `f_ftello` |
| `_fwrite` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:564` | Handler `f_fwrite` |
| `_getc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:574` | Handler `f_fgetc` |
| `_gettimeofday` | `libc++.1.dylib` | `present` | `src/runtime/libc/Time.cpp:137` | Handler `t_gettimeofday` |
| `_isalnum_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isalpha_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isblank_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iscntrl_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isdigit_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isgraph_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_islower_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isprint_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_ispunct_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isspace_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isupper_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswalpha_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswblank_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswcntrl_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswdigit_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswgraph_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswlower_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswprint_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswpunct_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswspace_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswupper_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_iswxdigit_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_isxdigit_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_localeconv` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Posix.cpp:227` | Handler `p_localeconv` |
| `_localeconv_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_lseek` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1151` | Handler `f_lseek` |
| `_mach_absolute_time` | `libc++.1.dylib` | `present` | `src/runtime/libc/Time.cpp:139` | Handler `t_mach_absolute_time` |
| `_mach_timebase_info` | `libc++.1.dylib` | `present` | `src/runtime/libc/Time.cpp:140` | Handler `t_mach_timebase_info` |
| `_malloc` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:336` | Handler `m_malloc` |
| `_mbrlen` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_mbrtowc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_mbsnrtowcs` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_mbsrtowcs` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_mbtowc` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_memchr` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:451` | Handler `s_memchr` |
| `_memcmp` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:449` | Handler `s_memcmp` |
| `_memcpy` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:443` | Handler `s_memcpy` |
| `_memmove` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:444` | Handler `s_memcpy` |
| `_memset` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:447` | Handler `s_memset` |
| `_nanosleep` | `libc++.1.dylib` | `present` | `src/runtime/libc/Time.cpp:143` | Handler `t_nanosleep` |
| `_newlocale` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_open` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1145` | Handler `f_open` |
| `_poll` | `libstdc++.6.dylib` | `present` | `src/runtime/libc/Net.cpp:591` | Handler `n_poll` |
| `_posix_memalign` | `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:342` | Handler `m_posix_memalign` |
| `_pthread_cond_broadcast` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:571` | `SupportStatus::Approx` — Wakes all current waiters and always reports success; waits poll for cancellation and stop |
| `_pthread_cond_destroy` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:573` | Handler `pc_destroy` |
| `_pthread_cond_init` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:565` | Handler `pc_init` |
| `_pthread_cond_signal` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:569` | `SupportStatus::Approx` — Wakes one current waiter and always reports success; waits poll for cancellation and stop |
| `_pthread_cond_timedwait` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:567` | Handler `pc_timedwait` |
| `_pthread_cond_wait` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:566` | Handler `pc_wait` |
| `_pthread_detach` | `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:601` | Handler `t_detach` |
| `_pthread_equal` | `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:603` | Handler `t_equal` |
| `_pthread_getspecific` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:586` | Handler `k_get` |
| `_pthread_join` | `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:600` | Handler `t_join` |
| `_pthread_key_create` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:584` | Handler `k_create` |
| `_pthread_key_delete` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:585` | Handler `k_delete` |
| `_pthread_mutex_destroy` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:558` | Handler `pm_destroy` |
| `_pthread_mutex_init` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:554` | Handler `pm_init` |
| `_pthread_mutex_lock` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:555` | Handler `pm_lock` |
| `_pthread_mutex_trylock` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:556` | Handler `pm_trylock` |
| `_pthread_mutex_unlock` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:557` | Handler `pm_unlock` |
| `_pthread_mutexattr_destroy` | `libc++.1.dylib` | `stubbed-dishonestly` | `src/runtime/libc/Pthread.cpp:562` | Handler `pma_ok` |
| `_pthread_mutexattr_init` | `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:559` | Handler `pma_init` |
| `_pthread_mutexattr_settype` | `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:560` | Handler `pma_settype` |
| `_pthread_once` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:583` | Handler `p_once` |
| `_pthread_self` | `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:602` | Handler `t_self` |
| `_pthread_setspecific` | `libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:587` | Handler `k_set` |
| `_putc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:562` | Handler `f_fputc` |
| `_read` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1147` | Handler `f_read` |
| `_realloc` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:338` | Handler `m_realloc` |
| `_sched_yield` | `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Pthread.cpp:612` | Handler `sched_yield_` |
| `_setlocale` | `libstdc++.6.dylib`, `libc++.1.dylib` | `stubbed-dishonestly` | `src/runtime/libc/Posix.cpp:226` | Handler `p_setlocale` returns fixed `"C"` string regardless of requested category/locale |
| `_snprintf` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:550` | Handler `f_snprintf` |
| `_snprintf_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_sprintf` | `libstdc++.6.dylib` | `present` | `src/runtime/libc/Stdio.cpp:548` | Handler `f_sprintf` |
| `_sscanf` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:594` | Handler `f_sscanf` |
| `_sscanf_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_stat` | `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1159` | Handler `f_stat` |
| `_strchr` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:477` | Handler `s_strchr` |
| `_strcmp` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:459` | Handler `s_strcmp` |
| `_strcoll` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:463` | Handler `s_strcoll` |
| `_strcoll_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strcpy` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:464` | Handler `s_strcpy` |
| `_strerror` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:492` | Handler `s_strerror` |
| `_strerror_r` | `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:493` | Handler `s_strerror_r` |
| `_strftime` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Time.cpp:152` | Handler `t_strftime` |
| `_strftime_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strlen` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:457` | Handler `s_strlen` |
| `_strncmp` | `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:460` | Handler `s_strncmp` |
| `_strtod` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:375` | Handler `c_strtod` |
| `_strtod_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strtof` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:376` | Handler `c_strtof` |
| `_strtof_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strtol` | `libstdc++.6.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:369` | Handler `c_strtol` |
| `_strtold` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strtold_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strtoll` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:371` | Handler `c_strtoll` |
| `_strtoll_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strtoul` | `libstdc++.6.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:370` | Handler `c_strtoul` |
| `_strtoull` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:373` | Handler `c_strtoull` |
| `_strtoull_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strxfrm` | `libstdc++.6.dylib`, `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_strxfrm_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_sysconf` | `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1121` | Handler `s_sysconf` |
| `_tolower_l` | `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:511` | Handler `c_tolower_l` |
| `_toupper_l` | `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:510` | Handler `c_toupper_l` |
| `_towlower_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_towupper_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_ungetc` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:577` | Handler `f_ungetc` |
| `_uselocale` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_vasprintf` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:557` | Handler `f_vasprintf` |
| `_vfprintf` | `libc++abi.dylib` | `present` | `src/runtime/libc/Stdio.cpp:547` | Handler `f_vfprintf` |
| `_vsnprintf` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:554` | Handler `f_vsnprintf` |
| `_vsscanf` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdio.cpp:595` | Handler `f_vsscanf` |
| `_wcrtomb` | `libstdc++.6.dylib`, `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcscmp` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:518` | Handler `w_wcscmp` |
| `_wcscoll` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcscoll_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcslen` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:513` | Handler `w_wcslen` |
| `_wcsnrtombs` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcsrtombs` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcstod` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:381` | Handler `c_wcstod` |
| `_wcstof` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:382` | Handler `c_wcstof` |
| `_wcstold` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcstoll` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:379` | Handler `c_wcstoll` |
| `_wcstoull` | `libc++.1.dylib` | `present` | `src/runtime/libc/Stdlib.cpp:380` | Handler `c_wcstoull` |
| `_wcsxfrm` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wcsxfrm_l` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wctob` | `libc++.1.dylib` | `missing` | `—` | Not registered in HydrogenVM symbol tables |
| `_wmemchr` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:527` | Handler `w_wmemchr` |
| `_wmemcmp` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:526` | Handler `w_wmemcmp` |
| `_wmemcpy` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:523` | Handler `w_wmemcpy` |
| `_wmemmove` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:524` | Handler `w_wmemcpy` |
| `_wmemset` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/String.cpp:525` | Handler `w_wmemset` |
| `_write` | `libstdc++.6.dylib`, `libc++.1.dylib` | `present` | `src/runtime/libc/System.cpp:1148` | Handler `f_write` |
