/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IW_SESSION_H
#define IW_SESSION_H
/* Hold C1ancher's hardware lease BEFORE opening epaper/evdev. Reuses an
 * inherited exclusive lease from c1pkg direct mode, or acquires a new one.
 * Failure is fatal: never attempt to out-refresh a competing display owner. */
int iw_session_open(void);
void iw_session_close(void);
#endif
