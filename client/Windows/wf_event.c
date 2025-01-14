/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Event Handling
 *
 * Copyright 2009-2011 Jay Sorg
 * Copyright 2010-2011 Vic Lee
 * Copyright 2010-2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <stdio.h>

#include <winpr/assert.h>
#include <winpr/sysinfo.h>
#include <freerdp/freerdp.h>

#include "wf_client.h"

#include "wf_gdi.h"
#include "wf_event.h"

#include <freerdp/event.h>

static HWND g_focus_hWnd = NULL;
static HWND g_main_hWnd = NULL;
static HWND g_parent_hWnd = NULL;

#define X_POS(lParam) ((UINT16)(lParam & 0xFFFF))
#define Y_POS(lParam) ((UINT16)((lParam >> 16) & 0xFFFF))

#define RESIZE_MIN_DELAY 200 /* minimum delay in ms between two resizes */

static BOOL wf_scale_blt(wfContext* wfc, HDC hdc, int x, int y, int w, int h, HDC hdcSrc, int x1,
                         int y1, DWORD rop);
static BOOL wf_scale_mouse_event(wfContext* wfc, UINT16 flags, UINT16 x, UINT16 y);
#if (_WIN32_WINNT >= 0x0500)
static BOOL wf_scale_mouse_event_ex(wfContext* wfc, UINT16 flags, UINT16 buttonMask, UINT16 x,
                                    UINT16 y);
#endif

static BOOL g_flipping_in;
static BOOL g_flipping_out;

// need to track states of these keys within keyboard hook proc func since GetAsyncKeyState() does not work inside of it
static BOOL alt_pressed = FALSE;
static BOOL ctrl_pressed = FALSE;
static BOOL shift_pressed = FALSE;
static BOOL win_pressed = FALSE;

static DWORD last_key_up = 0;
static DWORD last_key_up_time = 0;
static DWORD last_key_dn = 0;

static LPARAM last_mousemove_lParam;
static WPARAM last_NCACTIVATE_wParam;

static BOOL mod_key_down()
{
	return ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000)
	        || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)
			|| ctrl_pressed || alt_pressed || shift_pressed || win_pressed);
}

LRESULT CALLBACK wf_ll_kbd_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
	DWORD ext_proc_id = 0;

	wfContext* wfc = NULL;
	DWORD rdp_scancode;
	rdpInput* input;
	PKBDLLHOOKSTRUCT p = (PKBDLLHOOKSTRUCT)lParam;
	if (g_focus_hWnd)
	{
		DEBUG_KBD("Low-level keyboard hook, hWnd %X nCode %X wParam %X", g_focus_hWnd, nCode, wParam);
		// WLog_DBG("wf_event", "Low-level keyboard hook, hWnd %X vkCode %X wParam %X", g_focus_hWnd, p->vkCode, wParam);
	}

	// basically pass along the first keyup message to OS instead of having hook eat it so that OS keystate is not out of sync
	if (g_flipping_in)
	{
		// if (!mod_key_down())
		if ((
				// ((p->vkCode==VK_LSHIFT||p->vkCode==VK_LSHIFT) && wParam==WM_KEYUP) ||
				(wParam==WM_KEYUP || wParam==WM_SYSKEYUP)
			))
		{
			WLog_DBG("wf_event", "set g_flipping_in = FALSE");
			g_flipping_in = FALSE;
		}

		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}

	if (g_parent_hWnd && g_main_hWnd)
	{
		wfc = (wfContext*)GetWindowLongPtr(g_main_hWnd, GWLP_USERDATA);
		GUITHREADINFO gui_thread_info;
		gui_thread_info.cbSize = sizeof(GUITHREADINFO);
		HWND fg_win_hwnd = GetForegroundWindow();
		DWORD fg_win_thread_id = GetWindowThreadProcessId(fg_win_hwnd, &ext_proc_id);
		BOOL result = GetGUIThreadInfo(fg_win_thread_id, &gui_thread_info);
		if (gui_thread_info.hwndFocus != wfc->hWndParent)
		{
			g_focus_hWnd = NULL;
			return CallNextHookEx(NULL, nCode, wParam, lParam);
		}

		g_focus_hWnd = g_main_hWnd;
	}

	if (g_focus_hWnd && (nCode == HC_ACTION))
	{
		switch (wParam)
		{
			case WM_KEYUP:
			case WM_SYSKEYUP:
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:

				if (!wfc)
					wfc = (wfContext*)GetWindowLongPtr(g_focus_hWnd, GWLP_USERDATA);
				// p = (PKBDLLHOOKSTRUCT)lParam;

				if (!wfc || !p)
					return 1;

				input = wfc->common.context.input;
				rdp_scancode = MAKE_RDP_SCANCODE((BYTE)p->scanCode, p->flags & LLKHF_EXTENDED);
				DEBUG_KBD("keydown %d scanCode 0x%08lX flags 0x%08lX vkCode 0x%08lX",
				          (wParam == WM_KEYDOWN), p->scanCode, p->flags, p->vkCode);

				switch (p->vkCode)
				{
					case VK_LSHIFT:
					case VK_RSHIFT:
						shift_pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
						break;
					case VK_LWIN:
					case VK_RWIN:
						win_pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
						break;
					
					case VK_LMENU:
					case VK_RMENU:
						alt_pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
						DEBUG_KBD("alt_pressed=%d", alt_pressed);

						if ((wParam == WM_SYSKEYUP || wParam == WM_KEYUP) &&
						    (last_key_up == VK_LCONTROL || last_key_up == VK_RCONTROL) &&
						    !ctrl_pressed && !last_key_dn)
						{
							last_key_up = 0;
							if (p->time - last_key_up_time <= 250)
							{
								WLog_DBG("wf_event", "DEFOCUS");
								SetForegroundWindow(FindWindow(L"Shell_TrayWnd", NULL));
							}
						}

						// last_key_up = ctrl_pressed ? p->vkCode : 0;
						break;

					case VK_LCONTROL:
					case VK_RCONTROL:
						ctrl_pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
						DEBUG_KBD("ctrl_pressed=%d", ctrl_pressed);

						if ((wParam == WM_SYSKEYUP || wParam == WM_KEYUP) &&
						    (last_key_up == VK_LMENU || last_key_up == VK_RMENU) && !alt_pressed &&
						    !last_key_dn)
						{
							last_key_up = 0;
							if (p->time - last_key_up_time <= 250)
							{
								WLog_DBG("wf_event", "DEFOCUS");
								SetForegroundWindow(FindWindow(L"Shell_TrayWnd", NULL));
							}
						}

						// last_key_up = alt_pressed ? p->vkCode : 0;
						break;
				}

				if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
				{
					last_key_up = p->vkCode;
					last_key_up_time = p->time;
					DEBUG_KBD("set last_key_up=0x%08lX time:%d", last_key_up, last_key_up_time);

					if ((!alt_pressed) && (!ctrl_pressed) && (last_key_up == p->vkCode))
					{
						last_key_dn = 0;
						DEBUG_KBD("set last_key_dn=0x%08lX", last_key_dn);
					}
				}
				else if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
				{
					// any key other than CTRL or ALT
					if (!(p->vkCode == VK_LCONTROL || p->vkCode == VK_RCONTROL ||
					      p->vkCode == VK_LMENU || p->vkCode == VK_RMENU))
					{
						last_key_dn = p->vkCode;
						DEBUG_KBD("set last_key_dn=0x%08lX", last_key_dn);
					}
				}

				if (wfc->fullscreen_toggle &&
				    ((p->vkCode == VK_RETURN) || (p->vkCode == VK_CANCEL)) &&
				    // p->flags & LLKHF_ALTDOWN)
				    (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
				    (GetAsyncKeyState(VK_MENU) & 0x8000)) /* could also use flags & LLKHF_ALTDOWN */
				{
					// if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
					if (wParam == WM_KEYDOWN)
					{
						wf_toggle_fullscreen(wfc);
						return 1;
					}
				}

				if (rdp_scancode == RDP_SCANCODE_NUMLOCK_EXTENDED)
				{
					/* Windows sends NumLock as extended - rdp doesn't */
					DEBUG_KBD("hack: NumLock (x45) should not be extended");
					rdp_scancode = RDP_SCANCODE_NUMLOCK;
				}
				else if (rdp_scancode == RDP_SCANCODE_NUMLOCK)
				{
					/* Windows sends Pause as if it was a RDP NumLock (handled above).
					 * It must however be sent as a one-shot Ctrl+NumLock */
					if (wParam == WM_KEYDOWN)
					{
						DEBUG_KBD("Pause, sent as Ctrl+NumLock");
						freerdp_input_send_keyboard_event_ex(input, TRUE, RDP_SCANCODE_LCONTROL);
						freerdp_input_send_keyboard_event_ex(input, TRUE, RDP_SCANCODE_NUMLOCK);
						freerdp_input_send_keyboard_event_ex(input, FALSE, RDP_SCANCODE_LCONTROL);
						freerdp_input_send_keyboard_event_ex(input, FALSE, RDP_SCANCODE_NUMLOCK);
					}
					else
					{
						DEBUG_KBD("Pause up");
					}

					return 1;
				}
				else if (rdp_scancode == RDP_SCANCODE_RSHIFT_EXTENDED)
				{
					DEBUG_KBD("right shift (x36) should not be extended");
					rdp_scancode = RDP_SCANCODE_RSHIFT;
				}

				freerdp_input_send_keyboard_event_ex(input, !(p->flags & LLKHF_UP), rdp_scancode);

				if ((p->vkCode == VK_NUMLOCK || p->vkCode == VK_CAPITAL || p->vkCode == VK_SCROLL ||
				    p->vkCode == VK_KANA) && !wfc->common.context.settings->RemoteAssistanceMode) // assistance mode won't sync lock key states correctly so only send these key events to server
					DEBUG_KBD(
					    "lock keys are processed on client side too to toggle their indicators");
				else
				{

					if (g_flipping_out && !mod_key_down())
					{
						WLog_DBG("wf_event", "if (g_flipping_out){...}");
						g_flipping_out = FALSE;
						g_focus_hWnd = NULL;
						freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SuspendInput, TRUE);
					}

					return 1;
				}

				break;
		}
	}

	// if (g_flipping_out)
	// {
		// if (!mod_key_down())
		// {
			// WLog_DBG("wf_event", "if (g_flipping_out){...}");
			// g_flipping_out = FALSE;
			// g_focus_hWnd = NULL;
		// }
	// }

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static BOOL wf_scale_mouse_pos(wfContext* wfc, UINT16* x, UINT16* y)
{
	int ww, wh, dw, dh;
	rdpSettings* settings;

	if (!wfc || !x || !y)
		return FALSE;

	settings = wfc->common.context.settings;

	if (!settings)
		return FALSE;

	// mouse x,y coord from WM_MOUSEMOVE is actually signed, but x,y params for this func are unsigned
	// and if mouse position is left of window or above window then x or y would be negative
	// so interpret them as signed INT16 and if they would have been negative, then just make them zero
	// this is to avoid mouse position being sent as off of right side of screen when it is really off to left side, and off of bottom when it is really off of top
	if ( *(INT16 *)x < 0)
		*x = 0;
	if ( *(INT16 *)y < 0)
		*y = 0;

	// if UINTvalue > INT_MAX then INTvalue = UINTvalue - UINT_MAX - 1

	// WLog_DBG("wf_scale_mouse_pos", "%d", *x );
	// INT16* ix = (INT16 *)x;
	// WLog_DBG("wf_scale_mouse_pos", "%d", *ix );

	if (!wfc->client_width)
		wfc->client_width = settings->DesktopWidth;

	if (!wfc->client_height)
		wfc->client_height = settings->DesktopHeight;

	ww = wfc->client_width;
	wh = wfc->client_height;
	dw = settings->DesktopWidth;
	dh = settings->DesktopHeight;

	if (!settings->SmartSizing || ((ww == dw) && (wh == dh)))
	{
		*x += wfc->xCurrentScroll;
		*y += wfc->yCurrentScroll;
	}
	else
	{
		*x = *x * dw / ww + wfc->xCurrentScroll;
		*y = *y * dh / wh + wfc->yCurrentScroll;
	}

	return TRUE;
}

void wf_event_focus_in(wfContext* wfc)
{
	WLog_DBG("wf_event", "wf_event_focus_in");
	
	UINT16 syncFlags;
	rdpInput* input;
	POINT pt;
	RECT rc;
	input = wfc->common.context.input;
	syncFlags = 0;

	if (GetKeyState(VK_NUMLOCK))
		syncFlags |= KBD_SYNC_NUM_LOCK;

	if (GetKeyState(VK_CAPITAL))
		syncFlags |= KBD_SYNC_CAPS_LOCK;

	if (GetKeyState(VK_SCROLL))
		syncFlags |= KBD_SYNC_SCROLL_LOCK;

	if (GetKeyState(VK_KANA))
		syncFlags |= KBD_SYNC_KANA_LOCK;

	input->FocusInEvent(input, syncFlags);

	// only if assistance mode and server key state does not match client keystate
	// if (GetKeyState(VK_CAPITAL))
	// {
		// freerdp_input_send_keyboard_event_ex(input, TRUE, RDP_SCANCODE_CAPSLOCK);
		// freerdp_input_send_keyboard_event_ex(input, FALSE, RDP_SCANCODE_CAPSLOCK);
	// }

	/* send pointer position if the cursor is currently inside our client area */
	GetCursorPos(&pt);
	ScreenToClient(wfc->hwnd, &pt);
	GetClientRect(wfc->hwnd, &rc);

	if (pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom)
	{
		wf_scale_mouse_pos(wfc, &((UINT16)pt.x), &((UINT16)pt.y));
		input->MouseEvent(input, PTR_FLAGS_MOVE, (UINT16)pt.x, (UINT16)pt.y);
	}
}

static BOOL wf_event_process_WM_MOUSEWHEEL(wfContext* wfc, HWND hWnd, UINT Msg, WPARAM wParam,
                                           LPARAM lParam, BOOL horizontal, UINT16 x, UINT16 y)
{
	int delta;
	UINT16 flags = 0;
	rdpInput* input;

	WINPR_ASSERT(wfc);

	input = wfc->common.context.input;
	WINPR_ASSERT(input);

	DefWindowProc(hWnd, Msg, wParam, lParam);
	delta = ((signed short)HIWORD(wParam)); /* GET_WHEEL_DELTA_WPARAM(wParam); */

	if (horizontal)
		flags |= PTR_FLAGS_HWHEEL;
	else
		flags |= PTR_FLAGS_WHEEL;

	if (delta < 0)
	{
		flags |= PTR_FLAGS_WHEEL_NEGATIVE;
		/* 9bit twos complement, delta already negative */
		delta = 0x100 + delta;
	}

	flags |= delta;
	return wf_scale_mouse_event(wfc, flags, x, y);
}

static void wf_sizing(wfContext* wfc, WPARAM wParam, LPARAM lParam)
{
	WLog_VRB("wf_event", "wf_sizing()");

	rdpSettings* settings = wfc->common.context.settings;
	// Holding the CTRL key down while resizing the window will force the desktop aspect ratio.
	LPRECT rect;

	if ((settings->SmartSizing || settings->DynamicResolutionUpdate) &&
	    (GetAsyncKeyState(VK_CONTROL) & 0x8000))
	{
		rect = (LPRECT)wParam;

		switch (lParam)
		{
			case WMSZ_LEFT:
			case WMSZ_RIGHT:
			case WMSZ_BOTTOMRIGHT:
				// Adjust height
				rect->bottom = rect->top + settings->DesktopHeight * (rect->right - rect->left) /
				                               settings->DesktopWidth;
				break;

			case WMSZ_TOP:
			case WMSZ_BOTTOM:
			case WMSZ_TOPRIGHT:
				// Adjust width
				rect->right = rect->left + settings->DesktopWidth * (rect->bottom - rect->top) /
				                               settings->DesktopHeight;
				break;

			case WMSZ_BOTTOMLEFT:
			case WMSZ_TOPLEFT:
				// adjust width
				rect->left = rect->right - (settings->DesktopWidth * (rect->bottom - rect->top) /
				                            settings->DesktopHeight);
				break;
		}
	}
}

static void wf_send_resize(wfContext* wfc)
{
	WLog_VRB("wf_event", "wf_send_resize");

	RECT windowRect;
	int targetWidth = wfc->client_width;
	int targetHeight = wfc->client_height;
	rdpSettings* settings = wfc->common.context.settings;

	if (settings->DynamicResolutionUpdate && wfc->disp != NULL)
	{
		if (GetTickCount64() - wfc->lastSentDate > RESIZE_MIN_DELAY)
		{
			if (wfc->fullscreen)
			{
				GetWindowRect(wfc->hwnd, &windowRect);
				targetWidth = windowRect.right - windowRect.left;
				targetHeight = windowRect.bottom - windowRect.top;
			}
			if (settings->SmartSizingWidth != targetWidth ||
			    settings->SmartSizingHeight != targetHeight)
			{
				DISPLAY_CONTROL_MONITOR_LAYOUT layout = { 0 };

				layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
				layout.Top = layout.Left = 0;
				layout.Width = targetWidth;
				layout.Height = targetHeight;
				layout.Orientation = settings->DesktopOrientation;
				layout.DesktopScaleFactor = settings->DesktopScaleFactor;
				layout.DeviceScaleFactor = settings->DeviceScaleFactor;
				layout.PhysicalWidth = targetWidth;
				layout.PhysicalHeight = targetHeight;

				if (IFCALLRESULT(CHANNEL_RC_OK, wfc->disp->SendMonitorLayout, wfc->disp, 1,
				                 &layout) != CHANNEL_RC_OK)
				{
					WLog_ERR("", "SendMonitorLayout failed.");
				}
				settings->SmartSizingWidth = targetWidth;
				settings->SmartSizingHeight = targetHeight;
			}
			wfc->lastSentDate = GetTickCount64();
		}
	}
}

LRESULT CALLBACK wf_event_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	HDC hdc;
	LONG_PTR ptr;
	wfContext* wfc;
	int x, y, w, h;
	PAINTSTRUCT ps;
	BOOL processed;
	RECT windowRect;
	MINMAXINFO* minmax;
	SCROLLINFO si;
	processed = TRUE;
	ptr = GetWindowLongPtr(hWnd, GWLP_USERDATA);
	wfc = (wfContext*)ptr;

	if (wfc != NULL)
	{
		rdpInput* input = wfc->common.context.input;
		rdpSettings* settings = wfc->common.context.settings;

		if (!g_parent_hWnd && wfc->hWndParent)
			g_parent_hWnd = wfc->hWndParent;

		if (!g_main_hWnd)
			g_main_hWnd = wfc->hwnd;

		// WLog_DBG("wf_event", "%x", Msg);

		switch (Msg)
		{
			case WM_MOVE:
				WLog_VRB("wf_event", "WM_MOVE");
				if (!wfc->disablewindowtracking)
				{
					int x = (int)(short)LOWORD(lParam);
					int y = (int)(short)HIWORD(lParam);
					wfc->client_x = x;
					wfc->client_y = y;
				}

				break;

			case WM_GETMINMAXINFO:
				WLog_VRB("wf_event", "WM_GETMINMAXINFO");
				if (wfc->common.context.settings->SmartSizing ||
				    wfc->common.context.settings->DynamicResolutionUpdate)
				{
					processed = FALSE;
				}
				else
				{
					// Set maximum window size for resizing
					minmax = (MINMAXINFO*)lParam;

					// always use the last determined canvas diff, because it could be
					// that the window is minimized when this gets called
					// wf_update_canvas_diff(wfc);

					if (!wfc->fullscreen)
					{
						// add window decoration
						minmax->ptMaxTrackSize.x = settings->DesktopWidth + wfc->diff.x;
						minmax->ptMaxTrackSize.y = settings->DesktopHeight + wfc->diff.y;
					}
				}

				break;

			case WM_SIZING:
				WLog_VRB("wf_event", "WM_SIZING");
				wf_sizing(wfc, lParam, wParam);
				break;

			case WM_SIZE:
				WLog_VRB("wf_event", "WM_SIZE %d,%d", LOWORD(lParam), HIWORD(lParam));
				GetWindowRect(wfc->hwnd, &windowRect);

				if (!wfc->fullscreen)
				{
					wfc->client_width = LOWORD(lParam);
					wfc->client_height = HIWORD(lParam);
					wfc->client_x = windowRect.left;
					wfc->client_y = windowRect.top;
				}
				else
				{
					wfc->wasMaximized = TRUE;
					wf_send_resize(wfc);
				}

				if (wfc->client_width && wfc->client_height)
				{
					wf_size_scrollbars(wfc, LOWORD(lParam), HIWORD(lParam));

					// Workaround: when the window is maximized, the call to "ShowScrollBars"
					// returns TRUE but has no effect.
					if (wParam == SIZE_MAXIMIZED && !wfc->fullscreen)
					{
						SetWindowPos(wfc->hwnd, HWND_TOP, 0, 0, windowRect.right - windowRect.left,
						             windowRect.bottom - windowRect.top,
						             SWP_NOMOVE | SWP_FRAMECHANGED);
						wfc->wasMaximized = TRUE;
						wf_send_resize(wfc);
					}
					else if (wParam == SIZE_RESTORED && !wfc->fullscreen && wfc->wasMaximized)
					{
						wfc->wasMaximized = FALSE;
						wf_send_resize(wfc);
					}
				}

				break;

			case WM_EXITSIZEMOVE:
				WLog_VRB("wf_event", "WM_EXITSIZEMOVE");
				wf_size_scrollbars(wfc, wfc->client_width, wfc->client_height);
				wf_send_resize(wfc);
				break;

			case WM_ERASEBKGND:
				WLog_VRB("wf_event", "WM_ERASEBKGND");
				/* Say we handled it - prevents flickering */
				return (LRESULT)1;

			case WM_PAINT:
				WLog_VRB("wf_event", "WM_PAINT");

				hdc = BeginPaint(hWnd, &ps);
				x = ps.rcPaint.left;
				y = ps.rcPaint.top;
				w = ps.rcPaint.right - ps.rcPaint.left + 1;
				h = ps.rcPaint.bottom - ps.rcPaint.top + 1;
				wf_scale_blt(wfc, hdc, x, y, w, h, wfc->primary->hdc,
				             x - wfc->offset_x + wfc->xCurrentScroll,
				             y - wfc->offset_y + wfc->yCurrentScroll, SRCCOPY);
				EndPaint(hWnd, &ps);

				break;
#if (_WIN32_WINNT >= 0x0500)

			case WM_XBUTTONDOWN:
				wf_scale_mouse_event_ex(wfc, PTR_XFLAGS_DOWN, GET_XBUTTON_WPARAM(wParam),
				                        X_POS(lParam) - wfc->offset_x,
				                        Y_POS(lParam) - wfc->offset_y);
				break;

			case WM_XBUTTONUP:
				wf_scale_mouse_event_ex(wfc, 0, GET_XBUTTON_WPARAM(wParam),
				                        X_POS(lParam) - wfc->offset_x,
				                        Y_POS(lParam) - wfc->offset_y);
				break;
#endif

			case WM_MBUTTONDOWN:
				wf_scale_mouse_event(wfc, PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3,
				                     X_POS(lParam) - wfc->offset_x, Y_POS(lParam) - wfc->offset_y);
				break;

			case WM_MBUTTONUP:
				wf_scale_mouse_event(wfc, PTR_FLAGS_BUTTON3, X_POS(lParam) - wfc->offset_x,
				                     Y_POS(lParam) - wfc->offset_y);
				break;

			case WM_LBUTTONDOWN:
				wf_scale_mouse_event(wfc, PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1,
				                     X_POS(lParam) - wfc->offset_x, Y_POS(lParam) - wfc->offset_y);
				SetCapture(wfc->hwnd);
				break;

			case WM_LBUTTONUP:
				wf_scale_mouse_event(wfc, PTR_FLAGS_BUTTON1, X_POS(lParam) - wfc->offset_x,
				                     Y_POS(lParam) - wfc->offset_y);
				ReleaseCapture();
				break;

			case WM_RBUTTONDOWN:
				wf_scale_mouse_event(wfc, PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2,
				                     X_POS(lParam) - wfc->offset_x, Y_POS(lParam) - wfc->offset_y);
				break;

			case WM_RBUTTONUP:
				wf_scale_mouse_event(wfc, PTR_FLAGS_BUTTON2, X_POS(lParam) - wfc->offset_x,
				                     Y_POS(lParam) - wfc->offset_y);
				break;

			case WM_MOUSEMOVE:
				// ignore repeated mousemove events if mouse has not moved
				if (last_mousemove_lParam != lParam)
				{
					// WLog_DBG("WM_MOUSEMOVE", "%d,%d %d,%d %d,%d", (X_POS(lParam)), Y_POS(lParam), wfc->offset_x, wfc->offset_y, X_POS(lParam) - wfc->offset_x, Y_POS(lParam) - wfc->offset_y);
					
					wf_scale_mouse_event(wfc, PTR_FLAGS_MOVE, X_POS(lParam) - wfc->offset_x,
					                     Y_POS(lParam) - wfc->offset_y);
					last_mousemove_lParam = lParam;
				}
				break;
#if (_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)

			case WM_MOUSEWHEEL:
				wf_event_process_WM_MOUSEWHEEL(wfc, hWnd, Msg, wParam, lParam, FALSE,
				                               X_POS(lParam) - wfc->offset_x,
				                               Y_POS(lParam) - wfc->offset_y);
				break;
#endif
#if (_WIN32_WINNT >= 0x0600)

			case WM_MOUSEHWHEEL:
				wf_event_process_WM_MOUSEWHEEL(wfc, hWnd, Msg, wParam, lParam, TRUE,
				                               X_POS(lParam) - wfc->offset_x,
				                               Y_POS(lParam) - wfc->offset_y);
				break;
#endif

			case WM_SETCURSOR:
				if (LOWORD(lParam) == HTCLIENT && g_focus_hWnd)
					SetCursor(wfc->cursor);
				else
					DefWindowProc(hWnd, Msg, wParam, lParam);

				break;

			case WM_HSCROLL:
			{
				WLog_VRB("wf_event", "WM_HSCROLL");
				int xDelta;  // xDelta = new_pos - current_pos
				int xNewPos; // new position
				int yDelta = 0;

				switch (LOWORD(wParam))
				{
					// User clicked the scroll bar shaft left of the scroll box.
					case SB_PAGEUP:
						xNewPos = wfc->xCurrentScroll - 50;
						break;

					// User clicked the scroll bar shaft right of the scroll box.
					case SB_PAGEDOWN:
						xNewPos = wfc->xCurrentScroll + 50;
						break;

					// User clicked the left arrow.
					case SB_LINEUP:
						xNewPos = wfc->xCurrentScroll - 5;
						break;

					// User clicked the right arrow.
					case SB_LINEDOWN:
						xNewPos = wfc->xCurrentScroll + 5;
						break;

					// User dragged the scroll box.
					case SB_THUMBPOSITION:
						xNewPos = HIWORD(wParam);
						break;

					// user is dragging the scrollbar
					case SB_THUMBTRACK:
						xNewPos = HIWORD(wParam);
						break;

					default:
						xNewPos = wfc->xCurrentScroll;
				}

				// New position must be between 0 and the screen width.
				xNewPos = MAX(0, xNewPos);
				xNewPos = MIN(wfc->xMaxScroll, xNewPos);

				// If the current position does not change, do not scroll.
				if (xNewPos == wfc->xCurrentScroll)
					break;

				// Determine the amount scrolled (in pixels).
				xDelta = xNewPos - wfc->xCurrentScroll;
				// Reset the current scroll position.
				wfc->xCurrentScroll = xNewPos;
				// Scroll the window. (The system repaints most of the
				// client area when ScrollWindowEx is called; however, it is
				// necessary to call UpdateWindow in order to repaint the
				// rectangle of pixels that were invalidated.)
				ScrollWindowEx(wfc->hwnd, -xDelta, -yDelta, (CONST RECT*)NULL, (CONST RECT*)NULL,
				               (HRGN)NULL, (PRECT)NULL, SW_INVALIDATE);
				UpdateWindow(wfc->hwnd);
				// Reset the scroll bar.
				si.cbSize = sizeof(si);
				si.fMask = SIF_POS;
				si.nPos = wfc->xCurrentScroll;
				SetScrollInfo(wfc->hwnd, SB_HORZ, &si, TRUE);
			}
			break;

			case WM_VSCROLL:
			{
				WLog_VRB("wf_event", "WM_VSCROLL");
				int xDelta = 0;
				int yDelta;  // yDelta = new_pos - current_pos
				int yNewPos; // new position

				switch (LOWORD(wParam))
				{
					// User clicked the scroll bar shaft above the scroll box.
					case SB_PAGEUP:
						yNewPos = wfc->yCurrentScroll - 50;
						break;

					// User clicked the scroll bar shaft below the scroll box.
					case SB_PAGEDOWN:
						yNewPos = wfc->yCurrentScroll + 50;
						break;

					// User clicked the top arrow.
					case SB_LINEUP:
						yNewPos = wfc->yCurrentScroll - 5;
						break;

					// User clicked the bottom arrow.
					case SB_LINEDOWN:
						yNewPos = wfc->yCurrentScroll + 5;
						break;

					// User dragged the scroll box.
					case SB_THUMBPOSITION:
						yNewPos = HIWORD(wParam);
						break;

					// user is dragging the scrollbar
					case SB_THUMBTRACK:
						yNewPos = HIWORD(wParam);
						break;

					default:
						yNewPos = wfc->yCurrentScroll;
				}

				// New position must be between 0 and the screen height.
				yNewPos = MAX(0, yNewPos);
				yNewPos = MIN(wfc->yMaxScroll, yNewPos);

				// If the current position does not change, do not scroll.
				if (yNewPos == wfc->yCurrentScroll)
					break;

				// Determine the amount scrolled (in pixels).
				yDelta = yNewPos - wfc->yCurrentScroll;
				// Reset the current scroll position.
				wfc->yCurrentScroll = yNewPos;
				// Scroll the window. (The system repaints most of the
				// client area when ScrollWindowEx is called; however, it is
				// necessary to call UpdateWindow in order to repaint the
				// rectangle of pixels that were invalidated.)
				ScrollWindowEx(wfc->hwnd, -xDelta, -yDelta, (CONST RECT*)NULL, (CONST RECT*)NULL,
				               (HRGN)NULL, (PRECT)NULL, SW_INVALIDATE);
				UpdateWindow(wfc->hwnd);
				// Reset the scroll bar.
				si.cbSize = sizeof(si);
				si.fMask = SIF_POS;
				si.nPos = wfc->yCurrentScroll;
				SetScrollInfo(wfc->hwnd, SB_VERT, &si, TRUE);
			}
			break;

			case WM_SYSCOMMAND:
			{
				if (wParam == SYSCOMMAND_ID_SMARTSIZING)
				{
					WLog_VRB("wf_event", "WM_SYSCOMMAND SYSCOMMAND_ID_SMARTSIZING");

					HMENU hMenu = GetSystemMenu(wfc->hwnd, FALSE);
					freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SmartSizing,
					                          !wfc->common.context.settings->SmartSizing);
					CheckMenuItem(hMenu, SYSCOMMAND_ID_SMARTSIZING,
					              wfc->common.context.settings->SmartSizing ? MF_CHECKED
					                                                        : MF_UNCHECKED);
					if (!wfc->common.context.settings->SmartSizing)
					{
						SetWindowPos(wfc->hwnd, HWND_TOP, -1, -1,
						             wfc->common.context.settings->DesktopWidth + wfc->diff.x,
						             wfc->common.context.settings->DesktopHeight + wfc->diff.y,
						             SWP_NOMOVE);
					}
					else
					{
						wf_size_scrollbars(wfc, wfc->client_width, wfc->client_height);
						wf_send_resize(wfc);
					}
				}
				else if (wParam == SYSCOMMAND_ID_REQUEST_CONTROL)
				{
					freerdp_client_encomsp_set_control(wfc->common.encomsp, TRUE);
				}
				else
				{
					processed = FALSE;
				}
			}
			break;

			default:
				processed = FALSE;
				break;
		}
	}
	else
	{
		processed = FALSE;
	}

	if (processed)
		return 0;

	switch (Msg)
	{
		case WM_DESTROY:
			WLog_DBG("wf_event", "WM_DESTROY");
			PostQuitMessage(WM_QUIT);
			break;

		case WM_SETFOCUS:
			WLog_DBG("wf_event", "WM_SETFOCUS");
			// GetActiveWindow(); GetFocus()
			// WLog_DBG("wf_event", "hWnd=%x, wfc->hwnd=%x, GetForegroundWindow()=%x, GetActiveWindow()=%x, GetFocus()=%x", hWnd, wfc->hwnd, GetForegroundWindow(), GetActiveWindow(), GetFocus());
			// SetActiveWindow(wfc->hwnd);
			// SetFocus(wfc->hwnd);
			// SetForegroundWindow(wfc->hwnd);
			// BringWindowToTop(wfc->hwnd);
			DEBUG_KBD("getting focus %X", hWnd);

			if (last_NCACTIVATE_wParam)
			{
				if (mod_key_down())
				{
					WLog_DBG("wf_event", "set g_flipping_in");
					g_flipping_in = TRUE;
				}

				g_focus_hWnd = hWnd;
				freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SuspendInput, FALSE);
				freerdp_set_focus(wfc->common.context.instance);
			}
			break;

		case WM_KILLFOCUS:
			WLog_DBG("wf_event", "WM_KILLFOCUS");
			// WLog_DBG("wf_event", "hWnd=%x, wfc->hwnd=%x, GetActiveWindow()=%x, GetFocus()=%x", hWnd, wfc->hwnd, GetActiveWindow(), GetFocus());
			
			if (g_focus_hWnd == hWnd && wfc && !wfc->fullscreen)
			{
				WLog_DBG("wf_event", "loosing focus %X", hWnd);

				if (mod_key_down())
				{
					WLog_DBG("wf_event", "set g_flipping_out");
					g_flipping_out = TRUE;
				}
				else
				{
					g_focus_hWnd = NULL;
					freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SuspendInput, TRUE);
				}
			}
			break;
/*
		case WM_ACTIVATE:
		{
			WLog_DBG("wf_event", "WM_ACTIVATE");
			int activate = (int)(short)LOWORD(wParam);

			if (activate != WA_INACTIVE)
			{
				// for some reason get this upon minimize via task bar click...
				// if (mod_key_down())
				// {
					// WLog_DBG("wf_event", "set g_flipping_in");
					// g_flipping_in = TRUE;
				// }

				// g_focus_hWnd = hWnd;
			}
			else
			{
				WLog_DBG("wf_event", "WM_ACTIVATE:WA_INACTIVE");
				
				if (mod_key_down())
				{
					WLog_DBG("wf_event", "set g_flipping_out");
					g_flipping_out = TRUE;
				}
				else
				{
					g_focus_hWnd = NULL;
					freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SuspendInput, TRUE);
				}

			}
		}
*/

		case WM_NCACTIVATE:

			// WLog_DBG("wf_event", "WM_NCACTIVATE");
			
			last_NCACTIVATE_wParam = wParam;
			
			if (wParam)
			{
				WLog_DBG("wf_event", "WM_NCACTIVATE:fActive=true");
				
				if (wfc->hwnd == GetForegroundWindow())
				{
					if (mod_key_down())
					{
						WLog_DBG("wf_event", "set g_flipping_in");
						g_flipping_in = TRUE;
					}

					g_focus_hWnd = hWnd;
					freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SuspendInput, FALSE);
					freerdp_set_focus(wfc->common.context.instance);
				}
			} else {
				WLog_DBG("wf_event", "WM_NCACTIVATE:fActive=false");
				
				if (g_focus_hWnd == hWnd && wfc && !wfc->fullscreen)
				{
					WLog_DBG("wf_event", "loosing focus %X", hWnd);

					if (mod_key_down())
					{
						WLog_DBG("wf_event", "set g_flipping_out");
						g_flipping_out = TRUE;
					}
					else
					{
						g_focus_hWnd = NULL;
						freerdp_settings_set_bool(wfc->common.context.settings, FreeRDP_SuspendInput, TRUE);
					}
				}
				
			}
			return DefWindowProc(hWnd, Msg, wParam, lParam);
			break;

		default:
			return DefWindowProc(hWnd, Msg, wParam, lParam);
			break;
	}

	return 0;
}

BOOL wf_scale_blt(wfContext* wfc, HDC hdc, int x, int y, int w, int h, HDC hdcSrc, int x1, int y1,
                  DWORD rop)
{
	rdpSettings* settings;
	UINT32 ww, wh, dw, dh;
	WINPR_ASSERT(wfc);

	settings = wfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (!wfc->client_width)
		wfc->client_width = settings->DesktopWidth;

	if (!wfc->client_height)
		wfc->client_height = settings->DesktopHeight;

	ww = wfc->client_width;
	wh = wfc->client_height;
	dw = settings->DesktopWidth;
	dh = settings->DesktopHeight;

	if (!ww)
		ww = dw;

	if (!wh)
		wh = dh;

	if (wfc->fullscreen || !settings->SmartSizing || (ww == dw && wh == dh))
	{
		return BitBlt(hdc, x, y, w, h, wfc->primary->hdc, x1, y1, SRCCOPY);
	}
	else
	{
		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, NULL);
		return StretchBlt(hdc, 0, 0, ww, wh, wfc->primary->hdc, 0, 0, dw, dh, SRCCOPY);
	}

	return TRUE;
}

static BOOL wf_scale_mouse_event(wfContext* wfc, UINT16 flags, UINT16 x, UINT16 y)
{
	MouseEventEventArgs eventArgs;

	WINPR_ASSERT(wfc);

	if (!wf_scale_mouse_pos(wfc, &x, &y))
		return FALSE;

	if (freerdp_client_send_button_event(&wfc->common, FALSE, flags, x, y))
		return FALSE;

	eventArgs.flags = flags;
	eventArgs.x = x;
	eventArgs.y = y;
	PubSub_OnMouseEvent(wfc->common.context.pubSub, &wfc->common.context, &eventArgs);
	return TRUE;
}

#if (_WIN32_WINNT >= 0x0500)
static BOOL wf_scale_mouse_event_ex(wfContext* wfc, UINT16 flags, UINT16 buttonMask, UINT16 x,
                                    UINT16 y)
{
	MouseEventExEventArgs eventArgs;

	WINPR_ASSERT(wfc);

	if (buttonMask & XBUTTON1)
		flags |= PTR_XFLAGS_BUTTON1;

	if (buttonMask & XBUTTON2)
		flags |= PTR_XFLAGS_BUTTON2;

	if (!wf_scale_mouse_pos(wfc, &x, &y))
		return FALSE;

	if (freerdp_client_send_extended_button_event(&wfc->common, FALSE, flags, x, y))
		return FALSE;

	eventArgs.flags = flags;
	eventArgs.x = x;
	eventArgs.y = y;
	PubSub_OnMouseEventEx(wfc->common.context.pubSub, &wfc->common.context, &eventArgs);
	return TRUE;
}
#endif
