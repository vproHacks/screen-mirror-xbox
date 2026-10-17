/* Force-included (/FI) into every UxPlay C file when building with MSVC. */
#ifndef AIRSERVER_FORCE_H
#define AIRSERVER_FORCE_H
#include <BaseTsd.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#endif
