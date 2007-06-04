/*
 * Copyright © 2005 Novell, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * Novell, Inc. not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior permission.
 * Novell, Inc. makes no representations about the suitability of this
 * software for any purpose. It is provided "as is" without express or
 * implied warranty.
 *
 * NOVELL, INC. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL NOVELL, INC. BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *
 * Authors: 
 *	- Original zoom plugin; David Reveman <davidr@novell.com>
 *	- Most features beyond basic zoom; 
 *	  Kristian Lyngstol <kristian@bohemians.org>
 *
 * This plugin offers basic zoom, and does not require input to be disabled
 * while zooming. Key features of the new version is a hopefully more generic
 * interface to the basic zoom features, allowing advanced control of the 
 * plugin based on events such as focus changes, cursor movement, manual
 * panning and similar. This plugin has also been inspired by the inputzoom.c
 * plugin written for the Beryl project and copyrighted to Dennis Kasprzyk and
 * Quinn Storm.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include <compiz.h>

static CompMetadata zoomMetadata;

static int displayPrivateIndex;

typedef enum _ZdOpt
{
    DOPT_INITIATE = 0,
    DOPT_IN,
    DOPT_OUT,
    DOPT_SPECIFIC_1,
    DOPT_SPECIFIC_2,
    DOPT_SPECIFIC_3,
    DOPT_SPECIFIC_LEVEL_1,
    DOPT_SPECIFIC_LEVEL_2,
    DOPT_SPECIFIC_LEVEL_3,
    DOPT_SPECIFIC_TARGET_FOCUS,
    DOPT_PAN_LEFT,
    DOPT_PAN_RIGHT,
    DOPT_PAN_UP,
    DOPT_PAN_DOWN,
    DOPT_FIT_TO_WINDOW,
    DOPT_CENTER_MOUSE,
    DOPT_FIT_TO_ZOOM,
    DOPT_NUM
} ZoomDisplayOptions;

typedef enum _ZsOpt
{
    SOPT_FOLLOW_FOCUS = 0,
    SOPT_SPEED,
    SOPT_TIMESTEP,
    SOPT_ZOOM_FACTOR,
    SOPT_FILTER_LINEAR,
    SOPT_SYNC_MOUSE,
    SOPT_POLL_INTERVAL,
    SOPT_FOCUS_DELAY,
    SOPT_PAN_FACTOR,
    SOPT_FOCUS_FIT_WINDOW,
    SOPT_ALLWAYS_FOCUS_FIT_WINDOW,
    SOPT_SCALE_MOUSE,
    SOPT_HIDE_ORIGINAL_MOUSE,
    SOPT_NUM
} ZoomScreenOptions;

typedef struct _CursorTexture
{
    Bool isSet;
    GLuint texture;
    CompScreen *screen;
    int width;
    int height;
    int hotX;
    int hotY;
} CursorTexture;

typedef struct _ZoomDisplay {
    int		    screenPrivateIndex;
    HandleEventProc handleEvent;
    Bool fixesSupported;
    int fixesEventBase;
    int fixesErrorBase;
    Bool canHideCursor;
    CompOption opt[DOPT_NUM];
} ZoomDisplay;

typedef struct _ZoomScreen {
    PreparePaintScreenProc	 preparePaintScreen;
    DonePaintScreenProc		 donePaintScreen;
    PaintOutputProc		 paintOutput;
    SetScreenOptionForPluginProc setScreenOptionForPlugin;
    CompOption opt[SOPT_NUM];
    CompTimeoutHandle mouseIntervalTimeoutHandle;
    float pointerSensitivity;
    GLfloat currentZoom;
    GLfloat newZoom;
    GLfloat xVelocity;
    GLfloat yVelocity;
    GLfloat zVelocity;
    GLfloat xTranslate; // Target (Modify this for fluent movement)
    GLfloat yTranslate;
    GLfloat realXTranslate; // Real, unadjusted (Modify this too for instant)
    GLfloat realYTranslate;
    GLfloat xtrans; // Real, adjusted (Don't modify these.)
    GLfloat ytrans;
    GLfloat ztrans;
    Bool moving; 
    int mouseX;
    int mouseY;
    XPoint savedPointer;
    Bool   grabbed;
    float maxTranslate;
    int zoomOutput;
    time_t lastChange;
    CursorTexture cursor;
    Bool cursorInfoSelected;
    Bool showScaled;
    Bool cursorHidden;
    Bool hideNormal;
} ZoomScreen;

/* These prototypes must be pre-defined since they cross-refference eachother
 * and thus makes it impossible to order them in a fashion that avoids this.
 */
static void updateMousePosition (CompScreen *s);
static void syncCenterToMouse (CompScreen *s);
static Bool updateMouseInterval (void *vs);
static void cursorZoomActive (CompScreen *s);
static void cursorZoomInactive (CompScreen *s);
static void drawCursor (CompScreen *s, CompOutput *output, const CompTransform *transform);

#define GET_ZOOM_DISPLAY(d)				      \
    ((ZoomDisplay *) (d)->privates[displayPrivateIndex].ptr)

#define ZOOM_DISPLAY(d)		           \
    ZoomDisplay *zd = GET_ZOOM_DISPLAY (d)

#define GET_ZOOM_SCREEN(s, zd)				         \
    ((ZoomScreen *) (s)->privates[(zd)->screenPrivateIndex].ptr)

#define ZOOM_SCREEN(s)						        \
    ZoomScreen *zs = GET_ZOOM_SCREEN (s, GET_ZOOM_DISPLAY (s->display))

#define NUM_OPTIONS(s) (sizeof ((s)->opt) / sizeof (CompOption))

/* Adjust the velocity in the z-direction. 
 */
static int
adjustZoomVelocity (CompScreen *s, float chunk)
{
    float d, adjust, amount;
    ZOOM_SCREEN (s);

    d = (zs->newZoom - zs->currentZoom) * 75.0f;

    adjust = d * 0.002f;
    amount = fabs (d);
    if (amount < 1.0f)
	amount = 1.0f;
    else if (amount > 5.0f)
	amount = 5.0f;

    zs->zVelocity = (amount * zs->zVelocity + adjust) / (amount + 1.0f);

    if (fabs (d) < 0.1f && fabs (zs->zVelocity) < 0.005f)
    {
	zs->currentZoom = zs->newZoom;
	zs->zVelocity = 0.0f;
	return TRUE;
    }
    else
    {
	zs->currentZoom += (zs->zVelocity * chunk) /
	    s->redrawTime;
	return FALSE;
    }
}

/* Adjust the X/Y velocity based on target translation and real translation.
 */
static Bool
adjustXYVelocity (CompScreen *s, float chunk)
{
    ZOOM_SCREEN (s);
    if (zs->realXTranslate == zs->xTranslate && zs->realYTranslate == zs->yTranslate)
	return TRUE;

    float xdiff, ydiff;
    float xadjust, yadjust;
    float xamount, yamount;

    zs->xVelocity /= 1.25f;
    zs->yVelocity /= 1.25f;
    xdiff = (zs->xTranslate - zs->realXTranslate) * 75.0f;
    ydiff = (zs->yTranslate - zs->realYTranslate) * 75.0f;
    xadjust = xdiff * 0.002f;
    yadjust = ydiff * 0.002f;
    xamount = fabs (xdiff); 
    yamount = fabs (ydiff); 

    if (xamount < 1.0f) xamount = 1.0f;
    else if (xamount > 5.0) xamount = 5.0f;
    if (yamount < 1.0f) yamount = 1.0f;
    else if (yamount > 5.0) yamount = 5.0f;
    
    zs->xVelocity = (xamount * zs->xVelocity + xadjust) / (xamount + 1.0f);
    zs->yVelocity = (yamount * zs->yVelocity + yadjust) / (yamount + 1.0f);
    
    if ((fabs(xdiff) < 0.1f && fabs (zs->xVelocity) < 0.005f) && 
	(fabs(ydiff) < 0.1f && fabs (zs->yVelocity) < 0.005f))
    {
	zs->realXTranslate = zs->xTranslate;
	zs->realYTranslate = zs->yTranslate;
	zs->xVelocity = 0.0f;
	zs->yVelocity = 0.0f;
	return TRUE;
    }
    zs->realXTranslate += (zs->xVelocity * chunk) / s->redrawTime;
    zs->realYTranslate += (zs->yVelocity * chunk) / s->redrawTime;
    return FALSE;
}

/* Calculates the real translation to be applied in PaintScreen().
 * Needs cleaning...
 */
static void
zoomPreparePaintScreen (CompScreen *s,
			int	   msSinceLastPaint)
{
    ZOOM_SCREEN (s);

    if (zs->grabbed)
    {
	int   steps;
	float amount, chunk;

	amount = msSinceLastPaint * 0.05f *
	    zs->opt[SOPT_SPEED].value.f;
	steps  = amount / (0.5f * zs->opt[SOPT_TIMESTEP].value.f);
	if (!steps) steps = 1;
	chunk  = amount / (float) steps;
	while (steps--)
	{
	    adjustXYVelocity (s, chunk);
	    adjustZoomVelocity (s, chunk);

	    zs->ztrans = DEFAULT_Z_CAMERA * zs->currentZoom;
	    if (zs->ztrans <= 0.1f)
	    {
		zs->zVelocity = 0.0f;
		zs->ztrans = 0.1f;
	    }

	    zs->xtrans = -zs->realXTranslate * (1.0f - zs->currentZoom);
	    zs->ytrans = zs->realYTranslate * (1.0f - zs->currentZoom);

	    if (zs->newZoom == 1.0f)
	    {
		if (zs->currentZoom == 1.0f && zs->zVelocity == 0.0f)
		{
		    zs->xVelocity = zs->yVelocity = 0.0f;
		    zs->grabbed = FALSE;
		    zs->moving = FALSE;

		    break;
		}
	    }
	    if (zs->opt[SOPT_SYNC_MOUSE].value.b && zs->moving)
		syncCenterToMouse (s);

	    if (!zs->xVelocity && !zs->yVelocity && !zs->zVelocity)
		zs->moving = FALSE;

	}
    }

    UNWRAP (zs, s, preparePaintScreen);
    (*s->preparePaintScreen) (s, msSinceLastPaint);
    WRAP (zs, s, preparePaintScreen, zoomPreparePaintScreen);
}

/* Damage screen if we're still moving.
 */
static void
zoomDonePaintScreen (CompScreen *s)
{
    ZOOM_SCREEN (s);

    if (zs->grabbed)
    {
	if (zs->currentZoom != zs->newZoom ||
	    zs->xVelocity || zs->yVelocity || zs->zVelocity)
	    damageScreen (s);
    }

    UNWRAP (zs, s, donePaintScreen);
    (*s->donePaintScreen) (s);
    WRAP (zs, s, donePaintScreen, zoomDonePaintScreen);
}

/* Apply the zoom if we are grabbed. 
 * Make sure to use the correct filter.
 */
static Bool
zoomPaintOutput (CompScreen		 *s,
		 const ScreenPaintAttrib *sAttrib,
		 const CompTransform	 *transform,
		 Region		         region,
		 CompOutput		 *output,
		 unsigned int		 mask)
{
    Bool status;

    ZOOM_SCREEN (s);

    if (zs->grabbed)
    {
	mask &= ~PAINT_SCREEN_REGION_MASK;
	mask |= PAINT_SCREEN_CLEAR_MASK;
    }

    if (zs->grabbed && zs->zoomOutput == output->id)
    {
	ScreenPaintAttrib sa = *sAttrib;
	int		  saveFilter;

	sa.xTranslate += zs->xtrans;
	sa.yTranslate += zs->ytrans;
	sa.zCamera = -zs->ztrans;

	/* hack to get sides rendered correctly */
	if (zs->xtrans > 0.0f)
	    sa.xRotate += 0.000001f;
	else
	    sa.xRotate -= 0.000001f;

	mask |= PAINT_SCREEN_TRANSFORMED_MASK;
	saveFilter = s->filter[SCREEN_TRANS_FILTER];

	if (zs->opt[SOPT_FILTER_LINEAR].value.b)
	    s->filter[SCREEN_TRANS_FILTER] = COMP_TEXTURE_FILTER_GOOD;
	else
	    s->filter[SCREEN_TRANS_FILTER] = COMP_TEXTURE_FILTER_FAST;

	UNWRAP (zs, s, paintOutput);
	status = (*s->paintOutput) (s, &sa, transform, region, output, mask);
	WRAP (zs, s, paintOutput, zoomPaintOutput);
	drawCursor (s, output, transform);

	s->filter[SCREEN_TRANS_FILTER] = saveFilter;
    }
    else
    {
	UNWRAP (zs, s, paintOutput);
	status = (*s->paintOutput) (s, sAttrib, transform, region, output,
				    mask);
	WRAP (zs, s, paintOutput, zoomPaintOutput);
    }

    return status;
}

/* Makes sure we're not attempting to translate too far.
 * We are restricted to 0.5 because 
 * */
static inline void
constrainZoomTranslate (CompScreen *s)
{
    ZOOM_SCREEN (s);
    if (zs->xTranslate > 0.5f)
	zs->xTranslate = 0.5f;
    else if (zs->xTranslate < -0.5f)
	zs->xTranslate = -0.5f;

    if (zs->yTranslate > 0.5f)
	zs->yTranslate = 0.5f;
    else if (zs->yTranslate < -0.5f)
	zs->yTranslate = -0.5f;

    if (zs->xTranslate < -zs->maxTranslate)
	zs->xTranslate = -zs->maxTranslate;
    else if (zs->xTranslate > zs->maxTranslate)
	zs->xTranslate = zs->maxTranslate;

    if (zs->yTranslate < -zs->maxTranslate)
	zs->yTranslate = -zs->maxTranslate;
    else if (zs->yTranslate > zs->maxTranslate)
	zs->yTranslate = zs->maxTranslate;
}

/* Functions for adjusting the zoomed area.
 * These are the core of the zoom plugin; Anything wanting
 * to adjust the zoomed area must use setCenter or setZoomArea
 * and setScale. 
 */

/* Sets the center of the zoom area to X,Y.
 * We have to be able to warp the pointer here: If we are moved by
 * anything except mouse movement, we have to sync the
 * mouse pointer. This is to allow input, and is NOT necesarry
 * when input redirection is available to us.
 * The center is not the center of the screen. This is the target-center.
 */
static void
setCenter (CompScreen *s, int x, int y, Bool instant)
{
    ZOOM_SCREEN(s);
    CompOutput *o = &s->outputDev[zs->zoomOutput];

    zs->xTranslate = (float) 
	((x - o->region.extents.x1) - o->width  / 2) / (s->width);
    zs->yTranslate = (float) 
	((y - o->region.extents.y1) - o->height / 2) / (s->height);
    
    if (instant)
    {
	zs->realXTranslate = zs->xTranslate;
	zs->realYTranslate = zs->yTranslate;
	zs->yVelocity = 0.0f;
	zs->xVelocity = 0.0f;
        zs->moving = FALSE;
    } 
}

/* 
 * Zooms the area described. 
 * The math could probably be cleaned up, but should be correct now.
 */
static void
setZoomArea (CompScreen *s, int x, int y, int width, int height, Bool instant)
{
    ZOOM_SCREEN (s);
    if (zs->newZoom == 1.0f)  // Remove for divid-by-zero fun.
	return;
    zs->xTranslate = 
	 (float) -((s->width/2) - (x + (width/2)))
	/ (s->width);
    zs->xTranslate /= (1.0f - zs->newZoom);
    zs->yTranslate = 
	(float) -((s->height/2) - (y + (height/2))) 
	/ (s->height);
    zs->yTranslate /= (1.0f - zs->newZoom);
    zs->moving = TRUE;
    constrainZoomTranslate (s);

    if (instant)
    {
	zs->realXTranslate = zs->xTranslate;
	zs->realYTranslate = zs->yTranslate;
    }
}

/* Moves the zoom area to the window specified
 */
static void
zoomAreaToWindow (CompWindow *w)
{
    int left = w->serverX - w->input.left;
    int width = w->width + w->input.left + w->input.right; 
    int top = w->serverY - w->input.top;
    int height = w->height + w->input.top + w->input.bottom;
    setZoomArea (w->screen, left, top, width, height, FALSE);
}

/* Pans the zoomed area vertically/horisontaly by
 * value * zs->panFactor
 * Used both by key bindings and future mouse-based
 * panning.
 */
static void
panZoom (CompScreen *s, int xvalue, int yvalue)
{
    ZOOM_SCREEN (s);
    zs->xTranslate += zs->opt[SOPT_PAN_FACTOR].value.f * xvalue * zs->currentZoom;
    zs->yTranslate += zs->opt[SOPT_PAN_FACTOR].value.f * yvalue * zs->currentZoom;
    zs->moving = TRUE;
    constrainZoomTranslate (s);
}

/* Sets the zoom (or scale) level.
 */
static void
setScale(CompScreen *s, float x, float y)
{
    float value = x > y ? x : y;
    ZOOM_SCREEN(s);
    zs->moving = TRUE;
    if (value >= 1.0f) // DEFAULT_Z_CAMERA - (DEFAULT_Z_CAMERA / 10.0f))
    {
	value = 1.0f;
    } 
    else 
    {
	if (value * DEFAULT_Z_CAMERA < 0.1f)
	    value = zs->newZoom; 

	if (!zs->grabbed)
	{
	    zs->zoomOutput = outputDeviceForPoint (s, pointerX, pointerY);
	    zs->mouseIntervalTimeoutHandle = compAddTimeout(zs->opt[SOPT_POLL_INTERVAL].value.i, updateMouseInterval, s);
	}
	zs->grabbed = TRUE;
	cursorZoomActive (s);
    }
    if (value == 1.0f)
    {
	zs->xTranslate = 0.0f;
	zs->yTranslate = 0.0f;
	cursorZoomInactive (s);
    }
    zs->newZoom = value;
    damageScreen(s);
}

/* Mouse code...
 * This takes care of keeping the mouse in sync with the zoomed area and
 * vice versa. This is necesarry since we don't have input redirection (yet).
 * They are easily disabled.
 */

/* Syncs the center, based on translations, back to the mouse. 
 * This should be called when doing non-IR zooming and moving the zoom
 * area based on events other than mouse movement.
 */
static void
syncCenterToMouse (CompScreen *s)
{
    ZOOM_SCREEN(s);
    CompOutput *o = &s->outputDev[zs->zoomOutput];

    float x = (float) ((zs->realXTranslate * s->width) + (o->width / 2) + o->region.extents.x1);
    float y = (float) ((zs->realYTranslate * s->height) + (o->height / 2) + o->region.extents.y1);

    if (((int)x != zs->mouseX || (int)y != zs->mouseY) && zs->grabbed && zs->newZoom != 1.0f)
    {
	warpPointer (s, x - pointerX , y - pointerY );
	zs->mouseX = x;
	zs->mouseY = y;
    } 
}

/* Update the mouse position.
 * Based on the zoom engine in use, we will have to move the zoom area.
 * This might have to be added to a timer. 
 */
static void
updateMousePosition (CompScreen *s)
{
    Window root_return;
    Window child_return;
    int rootX, rootY;
    int winX, winY;
    unsigned int maskReturn;
    XQueryPointer (s->display->display, s->root,
		  &root_return, &child_return,
		  &rootX, &rootY, &winX, &winY, &maskReturn);


    ZOOM_SCREEN(s);
    if ((rootX != zs->mouseX || rootY != zs->mouseY))
    {
	if (rootX > s->width || rootY > s->height || s->root != root_return)
	    return;
	zs->mouseX = rootX;
	zs->mouseY = rootY;
	if (zs->opt[SOPT_SYNC_MOUSE].value.b && !zs->moving)
	{
	    zs->lastChange = time(NULL);
	    setCenter (s, rootX, rootY, TRUE);
	}
	damageScreen (s);
    }
}

/* Timeout handler to poll the mouse. Returns false (and thereby does not get
 * re-added to the queue) when zoom is not active.
 */
static Bool 
updateMouseInterval (void *vs)
{
    CompScreen *s = vs;
    ZOOM_SCREEN (s);

    if (!zs->grabbed)
    {
	zs->mouseIntervalTimeoutHandle = FALSE;
	return FALSE;
    }
    updateMousePosition(s);
    return TRUE;
}

/* Free a cursor
 */
static void
freeCursor (CursorTexture * cursor)
{
    if (!cursor->isSet)
	return;
	
    makeScreenCurrent (cursor->screen);
    cursor->isSet = FALSE;
    glDeleteTextures (1, &cursor->texture);
    cursor->texture = 0;
}

/* Draws the actual cursor. 
 * FIXME: Clean up the math.
 */
static void
drawCursor (CompScreen *s, CompOutput *output, const CompTransform *transform)
{
    ZOOM_SCREEN (s);
    if (zs->cursor.isSet)
    {
	CompTransform sTransform = *transform;
	transformToScreenSpace (s, output, -DEFAULT_Z_CAMERA, &sTransform);

        glPushMatrix ();
	glLoadMatrixf (sTransform.m);
	glTranslatef (zs->realXTranslate * s->width + s->width/2, 
		      zs->realYTranslate * s->height + s->height/2, 
		      0.0f);

	if (zs->currentZoom != 1.0f)
	{
	    float mx = (zs->mouseX ) - (zs->realXTranslate * s->width + s->width/2);
	    float my = (zs->mouseY ) - (zs->realYTranslate * s->height + s->height/2);
	    mx /= zs->currentZoom;
	    my /= zs->currentZoom;
	    glTranslatef (mx, my, 0.0f);
	}
	glScalef (1.0f / zs->currentZoom, 1.0f / zs->currentZoom, 1.0f);
	int x = -zs->cursor.hotX;
	int y = -zs->cursor.hotY;

	glEnable (GL_BLEND);
	glBindTexture (GL_TEXTURE_RECTANGLE_ARB, zs->cursor.texture);
	glEnable (GL_TEXTURE_RECTANGLE_ARB);

	glBegin (GL_QUADS);
	glTexCoord2d (0, 0);
	glVertex2f (x, y);
	glTexCoord2d (0, zs->cursor.height);
	glVertex2f (x, y + zs->cursor.height);
	glTexCoord2d (zs->cursor.width, zs->cursor.height);
	glVertex2f (x + zs->cursor.width, y + zs->cursor.height);
	glTexCoord2d (zs->cursor.width, 0);
	glVertex2f (x + zs->cursor.width, y);
	glEnd ();
	glDisable (GL_BLEND);
	glBindTexture (GL_TEXTURE_RECTANGLE_ARB, 0);
	glDisable (GL_TEXTURE_RECTANGLE_ARB);
	glPopMatrix ();
    }
}

/* The cursor needs an update */
static void
zoomUpdateCursor(CompScreen * s, CursorTexture * cursor)
{
    makeScreenCurrent (s);
    Display * dpy = s->display->display;

    glEnable (GL_TEXTURE_RECTANGLE_ARB);
    if (!cursor->isSet)
    {
	cursor->isSet = TRUE;
	cursor->screen = s;
	glGenTextures (1, &cursor->texture);
	glBindTexture (GL_TEXTURE_RECTANGLE_ARB, cursor->texture);
	glTexParameteri (GL_TEXTURE_RECTANGLE_ARB, 
			 GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_RECTANGLE_ARB, 
			 GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			 GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			 GL_TEXTURE_WRAP_T, GL_CLAMP);
    }

    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    cursor->width = ci->width;
    cursor->height = ci->height;
    cursor->hotX = ci->xhot;
    cursor->hotY = ci->yhot;
    unsigned char *pixels = malloc(ci->width * ci->height * 4);
    int i;
    for (i = 0; i < ci->width * ci->height; i++)
    {
	unsigned long pix = ci->pixels[i];
	pixels[i * 4] = pix & 0xff;
	pixels[(i * 4) + 1] = (pix >> 8) & 0xff;
	pixels[(i * 4) + 2] = (pix >> 16) & 0xff;
	pixels[(i * 4) + 3] = (pix >> 24) & 0xff;
    }

    glBindTexture (GL_TEXTURE_RECTANGLE_ARB, cursor->texture);
    glTexImage2D (GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, cursor->width,
		  cursor->height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture (GL_TEXTURE_RECTANGLE_ARB, 0);
    glDisable (GL_TEXTURE_RECTANGLE_ARB);
    XFree (ci);
    free (pixels);
}

/* We are no longer zooming the cursor, so display it.
 */
static void
cursorZoomInactive (CompScreen *s)
{
    ZOOM_DISPLAY (s->display);
    if (!zd->fixesSupported)
	return;
    ZOOM_SCREEN (s);

    if (zs->cursorInfoSelected)
    {
	zs->cursorInfoSelected = FALSE;
	XFixesSelectCursorInput (s->display->display, s->root, 0);
    }

    if (zs->cursor.isSet)
    {
	freeCursor (&zs->cursor);
    }

    if (zs->cursorHidden)
    {
	zs->cursorHidden = FALSE;
	XFixesShowCursor (s->display->display, s->root);
    }
}

/* Cursor zoom is active: We need to hide the original, 
 * register for Cursor notifies and display the new one.
 * This can be called multiple times, not just on initial
 * activation.
 */
static void
cursorZoomActive (CompScreen *s)
{
    ZOOM_DISPLAY (s->display);
    if (!zd->fixesSupported)
	return;
    ZOOM_SCREEN (s);
    if (!zs->opt[SOPT_SCALE_MOUSE].value.b)
	return;

    if (!zs->cursorInfoSelected)
    { 
	zs->cursorInfoSelected = TRUE;
        XFixesSelectCursorInput (s->display->display, s->root, 
				 XFixesDisplayCursorNotifyMask);
	zoomUpdateCursor (s, &zs->cursor);
    }
    if (zd->canHideCursor && !zs->cursorHidden && zs->opt[SOPT_HIDE_ORIGINAL_MOUSE].value.b)
    {
	zs->cursorHidden = TRUE;
	XFixesHideCursor (s->display->display, s->root);
    }
}

/* Zoom in to the area pointed to by the mouse.
 */
static Bool
zoomIn (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window     xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);

    if (s)
    {
	ZOOM_SCREEN (s);
	setScale (s, zs->newZoom/zs->opt[SOPT_ZOOM_FACTOR].value.f, -1.0f);
    }
    return TRUE;
}

/* Zoom to a specific level.
 * taget defines the target zoom level.
 * First set the scale level and mark the display as grabbed internally (to
 * catch the FocusIn event). Either target the focused window or the mouse,
 * depending on settings.
 * FIXME: A bit of a mess...
 */
static Bool
zoomSpecific (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption,
	float		target)
{
    CompScreen *s;
    Window     xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);

    if (s)
    {
	int   x, y;
	Bool wasZoomed;

	ZOOM_DISPLAY (d);
	ZOOM_SCREEN (s);

	wasZoomed = zs->newZoom == 1.0f;
	setScale (s, target, target);

	CompWindow *w;
	w = findWindowAtDisplay(d, d->activeWindow);
	if (zd->opt[DOPT_SPECIFIC_TARGET_FOCUS].value.b 
	    && w && w->screen->root == s->root)
	{
	    zoomAreaToWindow (w);
	}
	else
	{
	    x = getIntOptionNamed (option, nOption, "x", 0);
	    y = getIntOptionNamed (option, nOption, "y", 0);
	    setCenter (s, x, y, FALSE);
	}
    }
    return TRUE;
}

static Bool
zoomSpecific1 (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    ZOOM_DISPLAY (d);
    return zoomSpecific (d, action, state, option, nOption, 
			 zd->opt[DOPT_SPECIFIC_LEVEL_1].value.f);
}

static Bool
zoomSpecific2 (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    ZOOM_DISPLAY (d);
    return zoomSpecific (d, action, state, option, nOption, 
			 zd->opt[DOPT_SPECIFIC_LEVEL_2].value.f);
}

static Bool
zoomSpecific3 (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    ZOOM_DISPLAY (d);
    return zoomSpecific (d, action, state, option, nOption, 
			 zd->opt[DOPT_SPECIFIC_LEVEL_3].value.f);
}

/* Zooms to fit the active window to the screen without cutting
 * it off and targets it.
 */
static Bool
zoomToWindow (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (!s)
	return TRUE;
    CompWindow *w;
    w = findWindowAtDisplay (d, d->activeWindow);
    if (!w || w->screen->root != s->root)
	return TRUE;
    int width = w->width + w->input.left + w->input.right; 
    int height = w->height + w->input.top + w->input.bottom;
    setScale (s, (float) width/s->width, (float)  height/s->height);
    zoomAreaToWindow (w);
    return TRUE;
}

static Bool
zoomPanLeft (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (!s)
	return TRUE;
    panZoom (s, -1, 0);
    return TRUE;
}
static Bool
zoomPanRight (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (!s)
	return TRUE;
    panZoom (s, 1, 0);
    return TRUE;
}
static Bool
zoomPanUp (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (!s)
	return TRUE;
    panZoom (s, 0, -1);
    return TRUE;
}

static Bool
zoomPanDown (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (!s)
	return TRUE;
    panZoom (s, 0, 1);
    return TRUE;
}

/* Centers the mouse based on zoom level and translation.
 */
static Bool
zoomCenterMouse (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    Window xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (!s)
	return TRUE;
    ZOOM_SCREEN (s);
    warpPointer (s, 
		 (int) (s->width/2 - pointerX) + ((float)s->width * -zs->xtrans), 
		 (int) (s->height/2 - pointerY) + ((float)s->height * zs->ytrans));

    return TRUE;
}

/* Resize a window to fit the zoomed area.
 * This could probably do with some moving-stuff too.
 * IE: Move the zoom area afterwards. And ensure
 * the window isn't resized off-screen.
 */
static Bool
zoomFitWindowToZoom (CompDisplay     *d,
	CompAction      *action,
	CompActionState state,
	CompOption      *option,
	int		nOption)
{
    CompScreen *s;
    XWindowChanges xwc; 
    CompWindow * w;
    w = findWindowAtDisplay (d, d->activeWindow);
    if (!w)
	return TRUE;
    s = w->screen;
    ZOOM_SCREEN (s);
    xwc.x = w->serverX;
    xwc.y = w->serverY;
    xwc.width = (int) (s->width * zs->currentZoom - (int) ((w->input.left + w->input.right)));
    xwc.height = (int) (s->height * zs->currentZoom) - (int) ((w->input.top + w->input.bottom));
    sendSyncRequest (w);
    configureXWindow (w, (unsigned int) CWWidth | CWHeight, &xwc);
    return TRUE;
}

static Bool
zoomInitiate (CompDisplay     *d,
	      CompAction      *action,
	      CompActionState state,
	      CompOption      *option,
	      int	      nOption)
{
    zoomIn (d, action, state, option, nOption);

    if (state & CompActionStateInitKey)
	action->state |= CompActionStateTermKey;

    if (state & CompActionStateInitButton)
	action->state |= CompActionStateTermButton;

    return TRUE;
}

static Bool
zoomOut (CompDisplay     *d,
	 CompAction      *action,
	 CompActionState state,
	 CompOption      *option,
	 int	         nOption)
{
    CompScreen *s;
    Window     xid;
    xid = getIntOptionNamed (option, nOption, "root", 0);
    s = findScreenAtDisplay (d, xid);
    if (s)
    {
	ZOOM_SCREEN (s);
	setScale (s, zs->newZoom * zs->opt[SOPT_ZOOM_FACTOR].value.f, -1.0f);
    }

    return TRUE;
}

static Bool
zoomTerminate (CompDisplay     *d,
	       CompAction      *action,
	       CompActionState state,
	       CompOption      *option,
	       int	       nOption)
{
    CompScreen *s;
    Window     xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);

    for (s = d->screens; s; s = s->next)
    {
	ZOOM_SCREEN (s);

	if (xid && s->root != xid)
	    continue;

	if (zs->grabbed)
	{
	    zs->newZoom = 1.0f;
	    damageScreen (s);
	}
    }
    action->state &= ~(CompActionStateTermKey | CompActionStateTermButton);
    return FALSE;
}

/* Fetches focus changes and adjusts the zoom area. 
 * The focus handeling should be moved into a separate function
 * before a release.
 * The lastMapped is a hack to ensure that newly mapped windows are
 * caught even if the grab that (possibly) triggered them affected
 * the mode. Windows created by a keybind (like creating a terminal
 * on a keybind) tends to trigger FocusIn events with mode other than
 * Normal. This works around this problem.
 */
static void
zoomHandleEvent (CompDisplay *d,
		 XEvent      *event)
{
    ZOOM_DISPLAY(d);
    CompWindow *w;
    CompScreen *s;
    static Window lastMapped = 0;
    switch (event->type) {
	case FocusIn:
	    if ((event->xfocus.mode != NotifyNormal) && (lastMapped != event->xfocus.window))
		break;
	    lastMapped = 0;
	    w = findWindowAtDisplay(d, event->xfocus.window);
	    if (w == NULL) 
		break;

	    if (w->id == d->activeWindow)
		break;
	    ZOOM_SCREEN (w->screen);
	    
	    if (time(NULL) - zs->lastChange < 
		zs->opt[SOPT_FOCUS_DELAY].value.i)
		break;
	    if (!zs->opt[SOPT_FOLLOW_FOCUS].value.b)
		break;
	    if (!zs->grabbed && !zs->opt[SOPT_ALLWAYS_FOCUS_FIT_WINDOW].value.b)
		break;
	    if (zs->opt[SOPT_FOCUS_FIT_WINDOW].value.b)
	    {
		int width = w->width + w->input.left + w->input.right; 
		int height = w->height + w->input.top + w->input.bottom;
		setScale (w->screen, (float) width/w->screen->width, (float)  height/w->screen->height);
	    }
	    zoomAreaToWindow (w);
	    break;
	case MapNotify:
	    lastMapped = event->xmap.window;
	    break;
	default:
	    if (event->type == zd->fixesEventBase + XFixesCursorNotify)
	    {
		XFixesCursorNotifyEvent *cev = (XFixesCursorNotifyEvent *) event;
		s = findScreenAtDisplay (d, cev->window);
		if (s)
		{
		    ZOOM_SCREEN(s);
		    if (zs->cursor.isSet)
			zoomUpdateCursor (s, &zs->cursor);
		}
	    }
	    break;
    }
    UNWRAP (zd, d, handleEvent);
    (*d->handleEvent) (d, event);
    WRAP (zd, d, handleEvent, zoomHandleEvent);
}

/* Settings etc, boring stuff */
static const CompMetadataOptionInfo zoomDisplayOptionInfo[] = {
    { "initiate", "action", 0, zoomInitiate, zoomTerminate },
    { "zoom_in", "action", 0, zoomIn, 0 },
    { "zoom_out", "action", 0, zoomOut, 0 },
    { "zoom_specific_1", "action", 0, zoomSpecific1, 0 },
    { "zoom_specific_2", "action", 0, zoomSpecific2, 0 },
    { "zoom_specific_3", "action", 0, zoomSpecific3, 0 },
    { "zoom_spec1", "float", "<min>0.1</min><max>1.0</max><default>1.0</default>", 0, 0 },
    { "zoom_spec2", "float", "<min>0.1</min><max>1.0</max><default>0.5</default>", 0, 0 },
    { "zoom_spec3", "float", "<min>0.1</min><max>1.0</max><default>0.2</default>", 0, 0 },
    { "spec_target_focus", "bool", "<default>true</default>", 0, 0 },
    { "pan_left", "action", 0, zoomPanLeft, 0 },
    { "pan_right", "action", 0, zoomPanRight, 0 },
    { "pan_up", "action", 0, zoomPanUp, 0 },
    { "pan_down", "action", 0, zoomPanDown, 0 },
    { "fit_to_window", "action", 0, zoomToWindow, 0 },
    { "center_mouse", "action", 0, zoomCenterMouse, 0 },
    { "fit_to_zoom", "action", 0, zoomFitWindowToZoom, 0 }
};

static const CompMetadataOptionInfo zoomScreenOptionInfo[] = {
    { "follow_focus", "bool", 0, 0, 0 },
    { "speed", "float", "<min>0.01</min>", 0, 0 },
    { "timestep", "float", "<min>0.1</min>", 0, 0 },
    { "zoom_factor", "float", "<min>1.01</min>", 0, 0 },
    { "filter_linear", "bool", 0, 0, 0 },
    { "sync_mouse", "bool", 0, 0, 0 },
    { "mouse_poll_interval", "int", "<min>1</min>", 0, 0 },
    { "follow_focus_delay", "int", "<min>0</min>", 0, 0 }, 
    { "pan_factor", "float", "<min>0.001</min><default>0.1</default>", 0, 0 },
    { "focus_fit_window", "bool", "<default>false</default>", 0, 0 },
    { "allways_focus_fit_window", "bool", "<default>false</default>", 0, 0 },
    { "scale_mouse", "bool", "<default>false</default>", 0, 0 },
    { "hide_original_mouse", "bool", "<default>false</default>", 0, 0 }
};

static void
zoomUpdateCubeOptions (CompScreen *s)
{
    CompPlugin *p;
    ZOOM_SCREEN (s);
    p = findActivePlugin ("cube");
    if (p && p->vTable->getScreenOptions)
    {
	CompOption *options, *option;
	int	   nOptions;

	options = (*p->vTable->getScreenOptions) (p, s, &nOptions);
	option = compFindOption (options, nOptions, "in", 0);
	if (option)
	    zs->maxTranslate = option->value.b ? 0.85f : 1.5f;
    }
}

static CompOption *
zoomGetScreenOptions (CompPlugin *plugin,
		      CompScreen *screen,
		      int	 *count)
{
    ZOOM_SCREEN (screen);

    *count = NUM_OPTIONS (zs);
    return zs->opt;
}

static Bool
zoomSetScreenOption (CompPlugin      *plugin,
		     CompScreen      *screen,
		     char	     *name,
		     CompOptionValue *value)
{
    CompOption *o;
    int	       index;

    ZOOM_SCREEN (screen);

    o = compFindOption (zs->opt, NUM_OPTIONS (zs), name, &index);
    if (!o)
	return FALSE;

    return compSetScreenOption (screen, o, value);
}

static Bool
zoomSetScreenOptionForPlugin (CompScreen      *s,
			      char	      *plugin,
			      char	      *name,
			      CompOptionValue *value)
{
    Bool status;
    ZOOM_SCREEN (s);

    UNWRAP (zs, s, setScreenOptionForPlugin);
    status = (*s->setScreenOptionForPlugin) (s, plugin, name, value);
    WRAP (zs, s, setScreenOptionForPlugin, zoomSetScreenOptionForPlugin);

    if (status && strcmp (plugin, "cube") == 0)
	zoomUpdateCubeOptions (s);

    return status;
}

static CompOption *
zoomGetDisplayOptions (CompPlugin  *plugin,
		       CompDisplay *display,
		       int	   *count)
{
    ZOOM_DISPLAY (display);
    *count = NUM_OPTIONS (zd);
    return zd->opt;
}

static Bool
zoomSetDisplayOption (CompPlugin      *plugin,
		      CompDisplay     *display,
		      char	      *name,
		      CompOptionValue *value)
{
    CompOption *o;
    int	       index;
    ZOOM_DISPLAY (display);
    o = compFindOption (zd->opt, NUM_OPTIONS (zd), name, &index);
    if (!o)
	return FALSE;

    return compSetDisplayOption (display, o, value);
}


static Bool
zoomInitDisplay (CompPlugin  *p,
		 CompDisplay *d)
{
    ZoomDisplay *zd;
    zd = malloc (sizeof (ZoomDisplay));
    if (!zd)
	return FALSE;
    if (!compInitDisplayOptionsFromMetadata (d,
					     &zoomMetadata,
					     zoomDisplayOptionInfo,
					     zd->opt,
					     DOPT_NUM))
    {
	free (zd);
	return FALSE;
    }

    zd->screenPrivateIndex = allocateScreenPrivateIndex (d);
    if (zd->screenPrivateIndex < 0)
    {
	compFiniDisplayOptions (d, zd->opt, DOPT_NUM);
	free (zd);
	return FALSE;
    }

    zd->fixesSupported = XFixesQueryExtension(d->display, &zd->fixesEventBase,
			 &zd->fixesErrorBase);
    int minor, major;
    XFixesQueryVersion(d->display, &major, &minor);
    if (major >= 4)
	zd->canHideCursor = TRUE;
    else
	zd->canHideCursor = FALSE;

    WRAP (zd, d, handleEvent, zoomHandleEvent);
    d->privates[displayPrivateIndex].ptr = zd;
    return TRUE;
}

static void
zoomFiniDisplay (CompPlugin  *p,
		 CompDisplay *d)
{
    ZOOM_DISPLAY (d);
    freeScreenPrivateIndex (d, zd->screenPrivateIndex);
    UNWRAP (zd, d, handleEvent);
    compFiniDisplayOptions (d, zd->opt, DOPT_NUM);
    free (zd);
}

static Bool
zoomInitScreen (CompPlugin *p,
		CompScreen *s)
{
    ZoomScreen *zs;
    ZOOM_DISPLAY (s->display);
    zs = malloc (sizeof (ZoomScreen));
    if (!zs)
	return FALSE;

    if (!compInitScreenOptionsFromMetadata (s,
					    &zoomMetadata,
					    zoomScreenOptionInfo,
					    zs->opt,
					    SOPT_NUM))
    {
	free (zs);
	return FALSE;
    }

    zs->currentZoom = 1.0f;
    zs->newZoom = 1.0f;
    zs->xVelocity = 0.0f;
    zs->yVelocity = 0.0f;
    zs->zVelocity = 0.0f;
    zs->xTranslate = 0.0f;
    zs->yTranslate = 0.0f;
    zs->maxTranslate = 0.85f;
    zs->savedPointer.x = 0;
    zs->savedPointer.y = 0;
    zs->grabbed = FALSE;
    zs->zoomOutput = 0;
    zs->mouseX = -1;
    zs->mouseY = -1;
    zs->moving = FALSE;
    zs->hideNormal = FALSE;
    zs->showScaled = TRUE;
    zs->cursorInfoSelected = FALSE;
    zs->cursor.isSet = FALSE;
    zs->cursorHidden = FALSE;

    WRAP (zs, s, preparePaintScreen, zoomPreparePaintScreen);
    WRAP (zs, s, donePaintScreen, zoomDonePaintScreen);
    WRAP (zs, s, paintOutput, zoomPaintOutput);
    WRAP (zs, s, setScreenOptionForPlugin, zoomSetScreenOptionForPlugin);

    s->privates[zd->screenPrivateIndex].ptr = zs;
    zoomUpdateCubeOptions (s);
    return TRUE;
}

static void
zoomFiniScreen (CompPlugin *p,
		CompScreen *s)
{
    ZOOM_SCREEN (s);
    if (zs->mouseIntervalTimeoutHandle) 
	compRemoveTimeout (zs->mouseIntervalTimeoutHandle);

    UNWRAP (zs, s, preparePaintScreen);
    UNWRAP (zs, s, donePaintScreen);
    UNWRAP (zs, s, paintOutput);
    UNWRAP (zs, s, setScreenOptionForPlugin);

    compFiniScreenOptions (s, zs->opt, SOPT_NUM);
    free (zs);
}

static Bool
zoomInit (CompPlugin *p)
{
    if (!compInitPluginMetadataFromInfo (&zoomMetadata,
					 p->vTable->name,
					 zoomDisplayOptionInfo,
					 DOPT_NUM,
					 zoomScreenOptionInfo,
					 SOPT_NUM))
	return FALSE;

    displayPrivateIndex = allocateDisplayPrivateIndex ();
    if (displayPrivateIndex < 0)
    {
	compFiniMetadata (&zoomMetadata);
	return FALSE;
    }
    compAddMetadataFromFile (&zoomMetadata, p->vTable->name);
    return TRUE;
}

static void
zoomFini (CompPlugin *p)
{
    freeDisplayPrivateIndex (displayPrivateIndex);
    compFiniMetadata (&zoomMetadata);
}

static int
zoomGetVersion (CompPlugin *plugin,
		int	   version)
{
    return ABIVERSION;
}

static CompMetadata *
zoomGetMetadata (CompPlugin *plugin)
{
    return &zoomMetadata;
}

CompPluginVTable zoomVTable = {
    "zoom",
    zoomGetVersion,
    zoomGetMetadata,
    zoomInit,
    zoomFini,
    zoomInitDisplay,
    zoomFiniDisplay,
    zoomInitScreen,
    zoomFiniScreen,
    0, /* InitWindow */
    0, /* FiniWindow */
    zoomGetDisplayOptions,
    zoomSetDisplayOption,
    zoomGetScreenOptions,
    zoomSetScreenOption,
    0, /* Deps */
    0, /* nDeps */
    0, /* Features */
    0  /* nFeatures */
};

CompPluginVTable *
getCompPluginInfo (void)
{
    return &zoomVTable;
}
