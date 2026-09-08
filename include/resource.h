/* resource.h - resource and command identifiers */
#ifndef CTM_RESOURCE_H
#define CTM_RESOURCE_H

/* --------------------------------------------------------------- icons -- */
#define IDI_APPICON                     100

/* ------------------------------------------------------------- dialogs -- */
#define IDD_TABPAGE                     200
#define IDD_RUNDLG                      201

/* Run dialog controls */
#define IDC_RUN_ICON                    1001
#define IDC_RUN_TEXT                    1002
#define IDC_RUN_LABEL                   1003
#define IDC_RUN_COMBO                   1004
#define IDC_RUN_ADMIN                   1005

/* ------------------------------------------------------ shell controls -- */
#define IDC_TABCTRL                     1100
#define IDC_STATUSBAR                   1101
#define IDC_DASHBOARD                   1102

/* Applications tab (2000-2099) */
#define IDC_APPS_LIST                   2000
#define IDC_APPS_ENDTASK                2001
#define IDC_APPS_SWITCHTO               2002
#define IDC_APPS_NEWTASK                2003

/* Processes tab (2100-2199) */
#define IDC_PROC_LIST                   2100
#define IDC_PROC_ALLUSERS               2101
#define IDC_PROC_ENDPROCESS             2102
#define IDC_PROC_SEARCH                 2103
#define IDC_PROC_FILTER                 2104
#define IDC_PROC_CLEAR                  2105
#define IDC_PROC_EXPORT                 2106
#define IDC_PROC_DETAILS                2107
#define IDC_PROC_SUMMARY                2108
#define IDC_PROC_OPENLOCATION           2109
#define IDC_PROC_COPY                   2110

/* Services tab (2200-2299) */
#define IDC_SVC_LIST                    2200
#define IDC_SVC_SERVICES                2201

/* Performance tab (2300-2399) */
#define IDC_PERF_CPUGAUGE               2300
#define IDC_PERF_CPUHISTORY             2301
#define IDC_PERF_MEMGAUGE               2302
#define IDC_PERF_MEMHISTORY             2303
#define IDC_PERF_GRP_CPUUSAGE           2304
#define IDC_PERF_GRP_CPUHISTORY         2305
#define IDC_PERF_GRP_MEMUSAGE           2306
#define IDC_PERF_GRP_MEMHISTORY         2307
#define IDC_PERF_GRP_PHYSICAL           2308
#define IDC_PERF_GRP_KERNEL             2309
#define IDC_PERF_GRP_SYSTEM             2310
#define IDC_PERF_RESMON                 2311
#define IDC_PERF_LBL_FIRST              2320
#define IDC_PERF_PHYS_TOTAL             2320
#define IDC_PERF_PHYS_CACHED            2321
#define IDC_PERF_PHYS_AVAIL             2322
#define IDC_PERF_PHYS_FREE              2323
#define IDC_PERF_KERN_PAGED             2324
#define IDC_PERF_KERN_NONPAGED          2325
#define IDC_PERF_SYS_HANDLES            2326
#define IDC_PERF_SYS_THREADS            2327
#define IDC_PERF_SYS_PROCESSES          2328
#define IDC_PERF_SYS_UPTIME             2329
#define IDC_PERF_SYS_COMMIT             2330
#define IDC_PERF_LBL_LAST               2330
#define IDC_PERF_CAP_FIRST              2340   /* static captions 2340-2359 */

/* Networking tab (2400-2499) */
#define IDC_NET_GRAPHHOST               2400
#define IDC_NET_LIST                    2401

/* Users tab (2500-2599) */
#define IDC_USERS_LIST                  2500
#define IDC_USERS_DISCONNECT            2501
#define IDC_USERS_LOGOFF                2502
#define IDC_USERS_SENDMSG               2503

/* Sensors tab (2600-2699) */
#define IDC_SENS_GRAPHHOST              2600
#define IDC_SENS_LIST                   2601

/* ------------------------------------------------------ menu commands --- */
#define IDM_FILE_NEWTASK                40001
#define IDM_FILE_EXIT                   40002

#define IDM_OPTIONS_ALWAYSONTOP         40010
#define IDM_OPTIONS_MINIMIZEONUSE       40011
#define IDM_OPTIONS_HIDEWHENMIN         40012
#define IDM_OPTIONS_FULLACCOUNTNAME     40013

#define IDM_VIEW_REFRESH                40020
#define IDM_VIEW_SPEED_HIGH             40021
#define IDM_VIEW_SPEED_NORMAL           40022
#define IDM_VIEW_SPEED_LOW              40023
#define IDM_VIEW_SPEED_PAUSED           40024

#define IDM_VIEW_SELECTCOLUMNS          40030
#define IDM_VIEW_LARGEICONS             40031
#define IDM_VIEW_SMALLICONS             40032
#define IDM_VIEW_DETAILS                40033
#define IDM_VIEW_CPU_ONEGRAPH           40034
#define IDM_VIEW_CPU_PERCPU             40035
#define IDM_VIEW_SHOWKERNELTIMES        40036
#define IDM_VIEW_NET_BYTESSENT          40037
#define IDM_VIEW_NET_BYTESRECEIVED      40038
#define IDM_VIEW_NET_BYTESTOTAL         40039

#define IDM_WINDOWS_TILEHORZ            40050
#define IDM_WINDOWS_TILEVERT            40051
#define IDM_WINDOWS_MINIMIZE            40052
#define IDM_WINDOWS_MAXIMIZE            40053
#define IDM_WINDOWS_CASCADE             40054
#define IDM_WINDOWS_BRINGTOFRONT        40055

#define IDM_HELP_ABOUT                  40060

/* accelerator-only commands */
#define IDM_NEXT_TAB                    40070
#define IDM_PREV_TAB                    40071
#define IDM_TOGGLE_TINY                 40072
#define IDM_VIEW_TOGGLEPAUSE            40073
#define IDM_PROC_FIND                   40074
#define IDM_VIEW_PROCTREE               40075

/* Processes > Select Columns... popup: one command per toggleable column.
   Reserved range of its own; the popup uses TPM_RETURNCMD, but these must
   never alias IDM_FILE_* / IDM_OPTIONS_* if that ever changes. */
#define IDM_PROC_COL_FIRST              40200
#define IDM_PROC_COL_LAST               40219

/* tray menu */
#define IDM_TRAY_RESTORE                40080
#define IDM_TRAY_ALWAYSONTOP            40081
#define IDM_TRAY_EXIT                   40082

#endif /* CTM_RESOURCE_H */
