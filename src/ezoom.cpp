/*
 * Copyright © 2005 Novell, Inc.
 * Copyright (C) 2007, 2008 Kristian Lyngstøl
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
 * Author(s):
 *	- Original zoom plug-in; David Reveman <davidr@novell.com>
 *	- Most features beyond basic zoom;
 *	  Kristian Lyngstol <kristian@bohemians.org>
 *
 * Description:
 *
 * This plug-in offers zoom functionality with focus tracking,
 * fit-to-window actions, mouse panning, zoom area locking. Without
 * disabling input.
 *
 * Note on actual zoom process
 *
 * The animation is done in preparePaintScreen, while instant movements
 * are done by calling updateActualTranslate () after updating the
 * translations. This causes [xyz]trans to be re-calculated. We keep track
 * of each head separately.
 *
 * Note on input
 *
 * We can not redirect input yet, but this plug-in offers two fundamentally
 * different approaches to achieve input enabled zoom:
 *
 * 1.
 * Always have the zoomed area be in sync with the mouse cursor. This binds
 * the zoom area to the mouse position at any given time. It allows using
 * the original mouse cursor drawn by X, and is technically very safe.
 * First used in Beryl's inputzoom.
 *
 * 2.
 * Hide the real cursor and draw our own where it would be when zoomed in.
 * This allows us to navigate with the mouse without constantly moving the
 * zoom area. This is fairly close to what we want in the end when input
 * redirection is available.
 *
 * This second method has one huge issue, which is bugged XFixes. After
 * hiding the cursor once with XFixes, some mouse cursors will simply be
 * invisible. The Firefox loading cursor being one of them. 
 *
 * An other minor annoyance is that mouse sensitivity seems to increase as
 * you zoom in, since the mouse isn't really zoomed at all.
 *
 * Todo:
 *  - Different multi head modes
 */

#include "ezoom.h"

COMPIZ_PLUGIN_20090315 (ezoom, ZoomPluginVTable)

/* Checks if a specific screen grab exist. DO NOT USE THIS.
 * This is a temporary fix that SHOULD be removed asap.
 * See comments in drawCursor.
 */

static inline bool
dontuseScreengrabExist (char * grab)
{
    if (screen->otherGrabExist (grab, 0))
	return true;
    return false;
}

/* Check if the output is valid */
static inline bool
outputIsZoomArea (int out)
{
    ZOOM_SCREEN (screen);

    if (out < 0 || out >= zs->zooms.size ())
	return FALSE;
    return TRUE;
}

/* Check if zoom is active on the output specified */
static inline bool
isActive (int out)
{
    ZOOM_SCREEN (screen);

    if (!outputIsZoomArea (out))
	return FALSE;
    if (zs->grabbed & (1 << zs->zooms.at (out)->output))
	return TRUE;
    return FALSE;
}

/* Check if we are zoomed out and not going anywhere
 * (similar to isActive but based on actual zoom, not grab)
 */
static inline bool
isZoomed (int out)
{
    ZOOM_SCREEN (screen);

    if (!outputIsZoomArea (out))
	return FALSE;

    if (zs->zooms.at (out)->currentZoom != 1.0f 
	|| zs->zooms.at (out)->newZoom != 1.0f)
	return TRUE;

    if (zs->zooms.at (out)->zVelocity != 0.0f)
	return TRUE;

    return FALSE;
}

/* Returns the distance to the defined edge in zoomed pixels.  */
int
ZoomScreen::distanceToEdge (int out, ZoomScreen::ZoomEdge edge)
{
    int        x1,y1,x2,y2;
    CompOutput *o = &screen->outputDevs ().at (out);

    if (!isActive (out))
	return 0;
    convertToZoomedTarget (out, o->region ()->extents.x2, 
			   o->region ()->extents.y2, &x2, &y2);
    convertToZoomedTarget (out, o->region ()->extents.x1, 
			   o->region ()->extents.y1, &x1, &y1);
    switch (edge) 
    {
	case NORTH: return o->region ()->extents.y1 - y1;
	case SOUTH: return y2 - o->region ()->extents.y2;
	case EAST: return x2 - o->region ()->extents.x2;
	case WEST: return o->region ()->extents.x1 - x1;
    }
    return 0; // Never reached.
}

/* Update/set translations based on zoom level and real translate.  */
void
ZoomScreen::ZoomArea::updateActualTranslates ()
{
    xtrans = -realXTranslate * (1.0f - currentZoom);
    ytrans = realYTranslate * (1.0f - currentZoom);
}

/* Returns true if the head in question is currently moving.
 * Since we don't always bother resetting everything when
 * canceling zoom, we check for the condition of being completely
 * zoomed out and not zooming in/out first.
 */
bool
ZoomScreen::isInMovement (int out)
{
    if (zooms.at (out)->currentZoom == 1.0f &&
	zooms.at (out)->newZoom == 1.0f &&
	zooms.at (out)->zVelocity == 0.0f)
	return FALSE;
    if (zooms.at (out)->currentZoom != zooms.at (out)->newZoom ||
	zooms.at (out)->xVelocity || zooms.at (out)->yVelocity ||
	zooms.at (out)->zVelocity)
	return TRUE;
    if (zooms.at (out)->xTranslate != zooms.at (out)->realXTranslate ||
	zooms.at (out)->yTranslate != zooms.at (out)->realYTranslate)
	return TRUE;
    return FALSE;
}

/* Set the initial values of a zoom area.  */
ZoomScreen::ZoomArea::ZoomArea (int out) :
    output (out),
    viewport (~0),
    currentZoom (1.0f),
    newZoom (1.0f),
    xVelocity (0.0f),
    yVelocity (0.0f),
    zVelocity (0.0f),
    xTranslate (0.0f),
    yTranslate (0.0f),
    realXTranslate (0.0f),
    realYTranslate (0.0f),
    locked (false)
{
    updateActualTranslates ();
}

/* Adjust the velocity in the z-direction.  */
void
ZoomScreen::adjustZoomVelocity (int out, float chunk)
{
    float d, adjust, amount;

    d = (zooms.at (out)->newZoom - zooms.at (out)->currentZoom) * 75.0f;

    adjust = d * 0.002f;
    amount = fabs (d);
    if (amount < 1.0f)
	amount = 1.0f;
    else if (amount > 5.0f)
	amount = 5.0f;

    zooms.at (out)->zVelocity =
	(amount * zooms.at (out)->zVelocity + adjust) / (amount + 1.0f);

    if (fabs (d) < 0.1f && fabs (zooms.at (out)->zVelocity) < 0.005f)
    {
	zooms.at (out)->currentZoom = zooms.at (out)->newZoom;
	zooms.at (out)->zVelocity = 0.0f;
    }
    else
    {
	zooms.at (out)->currentZoom += (zooms.at (out)->zVelocity * chunk) /
	    cScreen->redrawTime ();
    }
}

/* Adjust the X/Y velocity based on target translation and real
 * translation. */
void
ZoomScreen::adjustXYVelocity (int out, float chunk)
{
    float xdiff, ydiff;
    float xadjust, yadjust;
    float xamount, yamount;

    zooms.at (out)->xVelocity /= 1.25f;
    zooms.at (out)->yVelocity /= 1.25f;
    xdiff =
	(zooms.at (out)->xTranslate - zooms.at (out)->realXTranslate) *
	75.0f;
    ydiff =
	(zooms.at (out)->yTranslate - zooms.at (out)->realYTranslate) *
	75.0f;
    xadjust = xdiff * 0.002f;
    yadjust = ydiff * 0.002f;
    xamount = fabs (xdiff);
    yamount = fabs (ydiff);

    if (xamount < 1.0f)
	    xamount = 1.0f;
    else if (xamount > 5.0) 
	    xamount = 5.0f;
    
    if (yamount < 1.0f) 
	    yamount = 1.0f;
    else if (yamount > 5.0) 
	    yamount = 5.0f;

    zooms.at (out)->xVelocity =
	(xamount * zooms.at (out)->xVelocity + xadjust) / (xamount + 1.0f);
    zooms.at (out)->yVelocity =
	(yamount * zooms.at (out)->yVelocity + yadjust) / (yamount + 1.0f);

    if ((fabs(xdiff) < 0.1f && fabs (zooms.at (out)->xVelocity) < 0.005f) &&
	(fabs(ydiff) < 0.1f && fabs (zooms.at (out)->yVelocity) < 0.005f))
    {
	zooms.at (out)->realXTranslate = zooms.at (out)->xTranslate;
	zooms.at (out)->realYTranslate = zooms.at (out)->yTranslate;
	zooms.at (out)->xVelocity = 0.0f;
	zooms.at (out)->yVelocity = 0.0f;
	return;
    }

    zooms.at (out)->realXTranslate +=
	(zooms.at (out)->xVelocity * chunk) / cScreen->redrawTime ();
    zooms.at (out)->realYTranslate +=
	(zooms.at (out)->yVelocity * chunk) / cScreen->redrawTime ();
}

/* Animate the movement (if any) in preparation of a paint screen.  */
void
ZoomScreen::preparePaint (int	   msSinceLastPaint)
{
    if (grabbed)
    {
	int   steps;
	float amount, chunk;

	amount = msSinceLastPaint * 0.05f * optionGetSpeed ();
	steps  = amount / (0.5f * optionGetTimestep ());
	if (!steps)
	       	steps = 1;
	chunk  = amount / (float) steps;
	while (steps--)
	{
	    int out;
	    for (out = 0; out < zooms.size (); out++)
	    {
		if (!isInMovement (out) || !isActive (out))
		    continue;

		adjustXYVelocity (out, chunk);
		adjustZoomVelocity (out, chunk);
		zooms.at (out)->updateActualTranslates ();
		if (!isZoomed (out))
		{
		    zooms.at (out)->xVelocity = zooms.at (out)->yVelocity =
			0.0f;
		    grabbed &= ~(1 << zooms.at (out)->output);
		}
	    }
	}
	if (optionGetSyncMouse ())
	    syncCenterToMouse ();
    }

    cScreen->preparePaint (msSinceLastPaint);
}

/* Damage screen if we're still moving.  */
void
ZoomScreen::donePaint ()
{
    if (grabbed)
    {
	int out;
	for (out = 0; out < zooms.size (); out++)
	{
	    if (isInMovement (out) && isActive (out))
	    {
		cScreen->damageScreen ();
		break;
	    }
	}
    }

    cScreen->donePaint ();
}
/* Draws a box from the screen coordinates inx1,iny1 to inx2,iny2 */
void
ZoomScreen::drawBox (const GLMatrix &transform, 
		     CompOutput          *output,
		     CompRect             box)
{
    GLMatrix zTransform = transform;
    int           x1,x2,y1,y2;
    int		  inx1, inx2, iny1, iny2;
    int	          out = output->id ();

    zTransform.toScreenSpace (output, -DEFAULT_Z_CAMERA);
    convertToZoomed (out, box.x1 (), box.y1 (), &inx1, &iny1);
    convertToZoomed (out, box.x2 (), box.y2 (), &inx2, &iny2);
    
    x1 = MIN (inx1, inx2);
    y1 = MIN (iny1, iny2);
    x2 = MAX (inx1, inx2);
    y2 = MAX (iny1, iny2);
    glPushMatrix ();
    glLoadMatrixf (zTransform.getMatrix ());
    glDisableClientState (GL_TEXTURE_COORD_ARRAY);
    glEnable (GL_BLEND);
    glColor4us (0x2fff, 0x2fff, 0x4fff, 0x4fff);
    glRecti (x1,y2,x2,y1);
    glColor4us (0x2fff, 0x2fff, 0x4fff, 0x9fff);
    glBegin (GL_LINE_LOOP);
    glVertex2i (x1, y1);
    glVertex2i (x2, y1);
    glVertex2i (x2, y2);
    glVertex2i (x1, y2);
    glEnd ();
    glColor4usv (defaultColor);
    glDisable (GL_BLEND);
    glEnableClientState (GL_TEXTURE_COORD_ARRAY);
    glPopMatrix ();
}
/* Apply the zoom if we are grabbed.
 * Make sure to use the correct filter.
 */
bool
ZoomScreen::glPaintOutput (const GLScreenPaintAttrib &attrib,
			   const GLMatrix	     &transform,
			   const CompRegion	     &region,
			   CompOutput 		     *output,
			   unsigned int		     mask)
{
    bool status;
    int	 out = output->id ();    

    if (isActive (out))
    {
	GLScreenPaintAttrib sa = attrib;
	int		    saveFilter;
	GLMatrix            zTransform = transform;

	mask &= ~PAINT_SCREEN_REGION_MASK;
	mask |= PAINT_SCREEN_CLEAR_MASK;

	zTransform.scale (1.0f / zooms.at (out)->currentZoom,
		          1.0f / zooms.at (out)->currentZoom,
		          1.0f);
	zTransform.translate (zooms.at (out)->xtrans,
			      zooms.at (out)->ytrans,
			      0); 

	mask |= PAINT_SCREEN_TRANSFORMED_MASK;
	saveFilter = gScreen->textureFilter ();

	if (optionGetFilterLinear ())
	    gScreen->setTextureFilter (GLTexture::Good);
	else
	    gScreen->setTextureFilter (GLTexture::Fast);

	status = gScreen->glPaintOutput (sa, zTransform, region, output,
									 mask);

	drawCursor (output, transform);

	gScreen->setTextureFilter (saveFilter);
    }
    else
    {
	status = gScreen->glPaintOutput (attrib, transform, region, output,
									mask);
    }
    if (grabIndex)
	drawBox (transform, output, box);

    return status;
}

/* Makes sure we're not attempting to translate too far.
 * We are restricted to 0.5 to not go beyond the end
 * of the screen/head.  */
static inline void
constrainZoomTranslate ()
{
    int out;
    ZOOM_SCREEN (screen);

    for (out = 0; out < zs->zooms.size (); out++)
    {
	if (zs->zooms.at (out)->xTranslate > 0.5f)
	    zs->zooms.at (out)->xTranslate = 0.5f;
	else if (zs->zooms.at (out)->xTranslate < -0.5f)
	    zs->zooms.at (out)->xTranslate = -0.5f;

	if (zs->zooms.at (out)->yTranslate > 0.5f)
	    zs->zooms.at (out)->yTranslate = 0.5f;
	else if (zs->zooms.at (out)->yTranslate < -0.5f)
	    zs->zooms.at (out)->yTranslate = -0.5f;
    }
}

/* Functions for adjusting the zoomed area.
 * These are the core of the zoom plug-in; Anything wanting
 * to adjust the zoomed area must use setCenter or setZoomArea
 * and setScale or front ends to them.  */

/* Sets the center of the zoom area to X,Y.
 * We have to be able to warp the pointer here: If we are moved by
 * anything except mouse movement, we have to sync the
 * mouse pointer. This is to allow input, and is NOT necessary
 * when input redirection is available to us or if we're cheating
 * and using a scaled mouse cursor to imitate IR.
 * The center is not the center of the screen. This is the target-center;
 * that is, it's the point that's the same regardless of zoom level.
 */
void
ZoomScreen::setCenter (int x, int y, bool instant)
{
    int         out = screen->outputDeviceForPoint (x, y);
    CompOutput  *o = &screen->outputDevs ().at (out);

    if (zooms.at (out)->locked)
	return;

    zooms.at (out)->xTranslate = (float)
	((x - o->x1 ()) - o->width ()  / 2) / (o->width ());
    zooms.at (out)->yTranslate = (float)
	((y - o->y1 ()) - o->height () / 2) / (o->height ());

    if (instant)
    {
	zooms.at (out)->realXTranslate = zooms.at (out)->xTranslate;
	zooms.at (out)->realYTranslate = zooms.at (out)->yTranslate;
	zooms.at (out)->yVelocity = 0.0f;
	zooms.at (out)->xVelocity = 0.0f;
	zooms.at (out)->updateActualTranslates ();
    }

    if (optionGetMousePan ())
	restrainCursor (out);
}

/* Zooms the area described.
 * The math could probably be cleaned up, but should be correct now. */
void
ZoomScreen::setZoomArea (int        x, 
	     		 int        y, 
	     		 int        width, 
	     		 int        height, 
	     		 Bool       instant)
{
    CompWindow::Geometry outGeometry (x, y, width, height, 0);
    int         out = screen->outputDeviceForGeometry (outGeometry);
    CompOutput  *o = &screen->outputDevs ().at (out);

    if (zooms.at (out)->newZoom == 1.0f)
	return;

    if (zooms.at (out)->locked)
	return;
    zooms.at (out)->xTranslate =
	 (float) -((o->width () / 2) - (x + (width / 2) - o->x1 ()))
	/ (o->width ());
    zooms.at (out)->xTranslate /= (1.0f - zooms.at (out)->newZoom);
    zooms.at (out)->yTranslate =
	(float) -((o->height () / 2) - (y + (height / 2) - o->y1 ()))
	/ (o->height ());
    zooms.at (out)->yTranslate /= (1.0f - zooms.at (out)->newZoom);
    constrainZoomTranslate ();

    if (instant)
    {
	zooms.at (out)->realXTranslate = zooms.at (out)->xTranslate;
	zooms.at (out)->realYTranslate = zooms.at (out)->yTranslate;
	zooms.at (out)->updateActualTranslates ();
    }

    if (optionGetMousePan ())
	restrainCursor (out);
}

/* Moves the zoom area to the window specified */
void
ZoomScreen::areaToWindow (CompWindow *w)
{
    int left = w->serverX () - w->input ().left;
    int width = w->width () + w->input ().left + w->input ().right;
    int top = w->serverY () - w->input ().top;
    int height = w->height ()  + w->input ().top + w->input ().bottom;
    
    setZoomArea (left, top, width, height, FALSE);
}

/* Pans the zoomed area vertically/horizontally by * value * zs->panFactor
 * TODO: Fix output. */
void
ZoomScreen::panZoom (int xvalue, int yvalue)
{
    int out;

    for (out = 0; out < zooms.size (); out++)
    {
	zooms.at (out)->xTranslate +=
	    optionGetPanFactor () * xvalue *
	    zooms.at (out)->currentZoom;
	zooms.at (out)->yTranslate +=
	    optionGetPanFactor () * yvalue *
	    zooms.at (out)->currentZoom;
    }

    constrainZoomTranslate ();
}

/* Enables polling of mouse position, and refreshes currently
 * stored values.
 */
void
ZoomScreen::enableMousePolling ()
{
    pollHandle.start ();
    lastChange = time(NULL);
    mouse = MousePoller::getCurrentPosition ();
}

/* Sets the zoom (or scale) level. 
 * Cleans up if we are suddenly zoomed out. 
 */
void
ZoomScreen::setScale (int out, float value)
{
    if (zooms.at (out)->locked)
	return;

    if (value >= 1.0f)
	value = 1.0f;
    else
    {
	if (!pollHandle.active ())
	    enableMousePolling ();
	grabbed |= (1 << zooms.at (out)->output);
	cursorZoomActive ();
    }

    if (value == 1.0f)
    {
	zooms.at (out)->xTranslate = 0.0f;
	zooms.at (out)->yTranslate = 0.0f;
	cursorZoomInactive ();
    }

    if (value < optionGetMinimumZoom ())
	value = optionGetMinimumZoom ();

    zooms.at (out)->newZoom = value;
    cScreen->damageScreen();
}

/* Sets the zoom factor to the bigger of the two floats supplied. 
 * Convenience function for setting the scale factor for an area.
 */
static inline void
setScaleBigger (int out, float x, float y)
{
    ZOOM_SCREEN (screen);
    zs->setScale (out, x > y ? x : y);
}

/* Mouse code...
 * This takes care of keeping the mouse in sync with the zoomed area and
 * vice versa. 
 * See heading for description.
 */

/* Syncs the center, based on translations, back to the mouse.
 * This should be called when doing non-IR zooming and moving the zoom
 * area based on events other than mouse movement.
 */
void
ZoomScreen::syncCenterToMouse ()
{
    int         x, y;
    int         out; 
    CompOutput  *o;

    out = screen->outputDeviceForPoint (mouse.x (), mouse.y ());
    o = &screen->outputDevs ().at (out);

    if (!isInMovement (out))
	return;

    x = (int) ((zooms.at (out)->realXTranslate * screen->width ()) +
	       (o->width () / 2) + o->x1 ());
    y = (int) ((zooms.at (out)->realYTranslate * screen->height ()) +
	       (o->height () / 2) + o->y1 ());

    if ((x != mouse.x () || y != mouse.y ())
	&& grabbed && zooms.at (out)->newZoom != 1.0f)
    {
	screen->warpPointer (x - pointerX , y - pointerY );
	mouse.setX (x);
	mouse.setY (y);
    }
}

/* Convert the point X,Y to where it would be when zoomed.  */
void
ZoomScreen::convertToZoomed (int        out, 
			     int        x, 
			     int        y, 
			     int        *resultX, 
			     int        *resultY)
{
    CompOutput  *o = &screen->outputDevs ()[out];
    ZoomArea    *za;

    za = zooms.at (out);
    x -= o->x1 ();
    y -= o->y1 ();
    *resultX = x - (za->realXTranslate *
		    (1.0f - za->currentZoom) * o->width ()) - o->width () / 2;
    *resultX /= za->currentZoom;
    *resultX += o->width () / 2;
    *resultX += o->x1 ();
    *resultY = y - (za->realYTranslate *
		    (1.0f - za->currentZoom) * o->height ()) - o->height ()/ 2;
    *resultY /= za->currentZoom;
    *resultY += o->height ()/ 2;
    *resultY += o->y1 ();
}

/* Same but use targeted translation, not real */
void
ZoomScreen::convertToZoomedTarget (int	  out,
			           int	  x,
			           int	  y,
			           int	  *resultX,
			           int	  *resultY)
{
    CompOutput  *o = &screen->outputDevs ().at (out);
    ZoomArea    *za;

    za = zooms.at (out);
    x -= o->x1 ();
    y -= o->y1 ();
    *resultX = x - (za->xTranslate *
		    (1.0f - za->newZoom) * o->width ()) - o->width () / 2;
    *resultX /= za->newZoom;
    *resultX += o->width () / 2;
    *resultX += o->x1 ();
    *resultY = y - (za->yTranslate *
		    (1.0f - za->newZoom) * o->height ()) - o->height ()/2;
    *resultY /= za->newZoom;
    *resultY += o->height () / 2;
    *resultY += o->y1 ();
}

/* Make sure the given point + margin is visible;
 * Translate to make it visible if necessary.
 * Returns false if the point isn't on a actively zoomed head
 * or the area is locked. */
bool
ZoomScreen::ensureVisibility (int x, int y, int margin)
{
    int         zoomX, zoomY;
    int         out;
    CompOutput  *o;

    out = screen->outputDeviceForPoint (x, y);
    if (!isActive (out))
	return FALSE;

    o = &screen->outputDevs ().at (out);
    convertToZoomedTarget (out, x, y, &zoomX, &zoomY);
    ZoomArea *za = zooms.at (out);
    if (za->locked)
	return FALSE;

#define FACTOR (za->newZoom / (1.0f - za->newZoom))
    if (zoomX + margin > o->x2 ())
	za->xTranslate +=
	    (FACTOR * (float) (zoomX + margin - o->x2 ())) /
	    (float) o->width ();
    else if (zoomX - margin < o->x1 ())
	za->xTranslate +=
	    (FACTOR * (float) (zoomX - margin - o->x1 ())) /
	    (float) o->width ();

    if (zoomY + margin > o->y2 ())
	za->yTranslate +=
	    (FACTOR * (float) (zoomY + margin - o->y2 ())) /
	    (float) o->height ();
    else if (zoomY - margin < o->y1 ())
	za->yTranslate +=
	    (FACTOR * (float) (zoomY - margin - o->y1 ())) /
	    (float) o->height ();
#undef FACTOR
    constrainZoomTranslate ();
    return TRUE;
}

/* Attempt to ensure the visibility of an area defined by x1/y1 and x2/y2.
 * See ensureVisibility () for details.
 *
 * This attempts to find the translations that leaves the biggest part of
 * the area visible. 
 *
 * gravity defines what part of the window that should get
 * priority if it isn't possible to fit all of it.
 */
void
ZoomScreen::ensureVisibilityArea (int         x1,
				  int         y1,
				  int         x2,
				  int         y2,
				  int         margin,
				  ZoomGravity gravity)
{
    int        targetX, targetY, targetW, targetH;
    int        out; 
    CompOutput *o; 
    
    out = screen->outputDeviceForPoint (x1 + (x2-x1/2), y1 + (y2-y1/2));
    o = &screen->outputDevs ().at (out);

#define WIDTHOK (float)(x2-x1) / (float)o->width () < zooms.at (out)->newZoom
#define HEIGHTOK (float)(y2-y1) / (float)o->height () < zooms.at (out)->newZoom

    if (WIDTHOK &&
	HEIGHTOK) {
	ensureVisibility (x1, y1, margin);
	ensureVisibility (x2, y2, margin);
	return;
    }

    switch (gravity)
    {
	case NORTHWEST:
	    targetX = x1;
	    targetY = y1;
	    if (WIDTHOK) 
		targetW = x2 - x1;
	    else 
		targetW = o->width () * zooms.at (out)->newZoom;
	    if (HEIGHTOK) 
		targetH = y2 - y1;
	    else 
		targetH = o->height () * zooms.at (out)->newZoom;
	    break;
	case NORTHEAST:
	    targetY = y1;
	    if (WIDTHOK)
	    {
		targetX = x1;
		targetW = x2-x1;
	    } 
	    else
	    {
		targetX = x2 - o->width () * zooms.at (out)->newZoom;
		targetW = o->width () * zooms.at (out)->newZoom;
	    }

	    if (HEIGHTOK)
		targetH = y2-y1;
	    else 
		targetH = o->height () * zooms.at (out)->newZoom;
	    break;
	case SOUTHWEST:
	    targetX = x1;
	    if (WIDTHOK)
		targetW = x2-x1;
	    else
		targetW = o->width () * zooms.at (out)->newZoom;
	    if (HEIGHTOK)
	    {
		targetY = y1;
		targetH = y2-y1;
	    } 
	    else
	    {
		targetY = y2 - (o->width () * zooms.at (out)->newZoom);
		targetH = o->width () * zooms.at (out)->newZoom;
	    }
	    break;
	case SOUTHEAST:
	    if (WIDTHOK)
	    {
		targetX = x1;
		targetW = x2-x1;
	    } 
	    else 
	    {
		targetW = o->width () * zooms.at (out)->newZoom;
		targetX = x2 - targetW;
	    }
	    
	    if (HEIGHTOK)
	    {
		targetY = y1;
		targetH = y2 - y1;
	    }
	    else
	    {
		targetH = o->height () * zooms.at (out)->newZoom;
		targetY = y2 - targetH;
	    }
	    break;
	case CENTER:
	    setCenter (x1 + (x2 - x1 / 2), y1 + (y2 - y1 / 2), FALSE);
	    return;
	    break;
    }

    setZoomArea (targetX, targetY, targetW, targetH, FALSE);
    return ;
}

/* Ensures that the cursor is visible on the given head.
 * Note that we check if currentZoom is 1.0f, because that often means that
 * mouseX and mouseY is not up-to-date (since the polling timer just
 * started).
 */
void
ZoomScreen::restrainCursor (int out)
{
    int         x1, y1, x2, y2, margin;
    int         diffX = 0, diffY = 0;
    int         north, south, east, west;
    float       z;
    CompOutput  *o = &screen->outputDevs ().at (out);

    z = zooms.at (out)->newZoom;
    margin = optionGetRestrainMargin ();
    north = distanceToEdge (out, NORTH);
    south = distanceToEdge (out, SOUTH);
    east = distanceToEdge (out, EAST);
    west = distanceToEdge (out, WEST);

    if (zooms.at (out)->currentZoom == 1.0f)
    {
	lastChange = time(NULL);
	mouse = MousePoller::getCurrentPosition ();
    }

    convertToZoomedTarget (out, mouse.x () - cursor.hotX, 
			   mouse.y () - cursor.hotY, &x1, &y1);
    convertToZoomedTarget 
	(out, 
	 mouse.x () - cursor.hotX + cursor.width, 
	 mouse.y () - cursor.hotY + cursor.height,
	 &x2, &y2);

    if ((x2 - x1 > o->x2 () - o->x1 ()) ||
       (y2 - y1 > o->y2 () - o->y1 ()))
	return;
    if (x2 > o->x2 () - margin && east > 0)
	diffX = x2 - o->x2 () + margin;
    else if (x1 < o->x1 () + margin && west > 0)
	diffX = x1 - o->x1 () - margin;

    if (y2 > o->y2 () - margin && south > 0)
	diffY = y2 - o->y2 () + margin;
    else if (y1 < o->y1 () + margin && north > 0) 
	diffY = y1 - o->y1 () - margin;

    if (abs(diffX)*z > 0  || abs(diffY)*z > 0)
	screen->warpPointer ((int) (mouse.x () - pointerX) -
						       (int) ((float)diffX * z),
		     	     (int) (mouse.y () - pointerY) -  
						      (int) ((float)diffY * z));
}

/* Check if the cursor is still visible.
 * We also make sure to activate/deactivate cursor scaling here
 * so we turn on/off the pointer if it moves from one head to another.
 * FIXME: Detect an actual output change instead of spamming.
 * FIXME: The second ensureVisibility (sync with restrain).
 */
void
ZoomScreen::cursorMoved ()
{
    int         out;

    out = screen->outputDeviceForPoint (mouse.x (), mouse.y ());
    if (isActive (out))
    {
	if (optionGetRestrainMouse ())
	    restrainCursor (out);

	if (optionGetMousePan ())
	{
	    ensureVisibilityArea (mouse.x () - cursor.hotX,
				  mouse.y () - cursor.hotY,
				  mouse.x () + cursor.width -
				  cursor.hotX,
				  mouse.y () + cursor.height - 
				  cursor.hotY,
				  optionGetRestrainMargin (),
				  NORTHWEST);
	}

	cursorZoomActive ();
    }
    else
    {
	cursorZoomInactive ();
    }
}

/* Update the mouse position.
 * Based on the zoom engine in use, we will have to move the zoom area.
 * This might have to be added to a timer.
 */
void
ZoomScreen::updateMousePosition (const CompPoint &p)
{
    int out; 
    mouse.setX (p.x ());
    mouse.setY (p.y ());
    out = screen->outputDeviceForPoint (mouse.x (), mouse.y ());
    lastChange = time(NULL);
    if (optionGetSyncMouse () && !isInMovement (out))
	setCenter (mouse.x (), mouse.y (), TRUE);
    cursorMoved ();
    cScreen->damageScreen ();
}

/* Timeout handler to poll the mouse. Returns false (and thereby does not
 * get re-added to the queue) when zoom is not active. */
void
ZoomScreen::updateMouseInterval (const CompPoint &p)
{
    updateMousePosition (p);

    if (!grabbed)
    {
	cursorMoved ();
	if (pollHandle.active ())
	    pollHandle.stop ();
    }
}

/* Free a cursor */
void
ZoomScreen::freeCursor (CursorTexture * cursor)
{
    if (!cursor->isSet)
	return;
	
    //makeScreenCurrent (cursor->screen); ??
    cursor->isSet = FALSE;
    glDeleteTextures (1, &cursor->texture);
    cursor->texture = 0;
}

/* Translate into place and draw the scaled cursor.  */
void
ZoomScreen::drawCursor (CompOutput          *output, 
	    		const GLMatrix      &transform)
{
    int         out = output->id ();

    if (cursor.isSet)
    {
	GLMatrix      sTransform = transform;
	float	      scaleFactor;
	int           ax, ay, x, y;	

	/* FIXME:
	 * This is a hack because these transformations are wrong when
	 * we're working exposed. Expo is capable of telling where the
	 * real mouse is despite zoom, so we don't have to disable the
	 * zoom. We do, however, have to show the original pointer.
	 */
	if (dontuseScreengrabExist ("expo"))
	{
	    cursorZoomInactive ();
	    return;
	}

	sTransform.toScreenSpace (output, -DEFAULT_Z_CAMERA);
	convertToZoomed (out, mouse.x (), mouse.y (), &ax, &ay);
        glPushMatrix ();
	glLoadMatrixf (sTransform.getMatrix ());
	glTranslatef ((float) ax, (float) ay, 0.0f);
	if (optionGetScaleMouseDynamic ()) 
	    scaleFactor = 1.0f / zooms.at (out)->currentZoom;
	else 
	    scaleFactor = 1.0f / optionGetScaleMouseStatic ();
	glScalef (scaleFactor,
		  scaleFactor,
		  1.0f);
	x = -cursor.hotX;
	y = -cursor.hotY;

	glEnable (GL_BLEND);
	glBindTexture (GL_TEXTURE_RECTANGLE_ARB, cursor.texture);
	glEnable (GL_TEXTURE_RECTANGLE_ARB);

	glBegin (GL_QUADS);
	glTexCoord2d (0, 0);
	glVertex2f (x, y);
	glTexCoord2d (0, cursor.height);
	glVertex2f (x, y + cursor.height);
	glTexCoord2d (cursor.width, cursor.height);
	glVertex2f (x + cursor.width, y + cursor.height);
	glTexCoord2d (cursor.width, 0);
	glVertex2f (x + cursor.width, y);
	glEnd ();
	glDisable (GL_BLEND);
	glBindTexture (GL_TEXTURE_RECTANGLE_ARB, 0);
	glDisable (GL_TEXTURE_RECTANGLE_ARB);
	glPopMatrix ();
    }
}

/* Create (if necessary) a texture to store the cursor,
 * fetch the cursor with XFixes. Store it.  */
void
ZoomScreen::updateCursor (CursorTexture * cursor)
{
    unsigned char *pixels;
    int           i;
    Display       *dpy = screen->dpy ();

    if (!cursor->isSet)
    {
	cursor->isSet = TRUE;
	cursor->screen = screen;
	// makeScreenCurrent (s); // ???
	glEnable (GL_TEXTURE_RECTANGLE_ARB);
	glGenTextures (1, &cursor->texture);
	glBindTexture (GL_TEXTURE_RECTANGLE_ARB, cursor->texture);

	if (optionGetFilterLinear () &&
	    gScreen->textureFilter () != GL_NEAREST) // ???
	{
	    glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			     GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	    glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			     GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
	    glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			     GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	    glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			     GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			 GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri (GL_TEXTURE_RECTANGLE_ARB,
			 GL_TEXTURE_WRAP_T, GL_CLAMP);
    } else {
	//makeScreenCurrent (cursor->screen); ???
	glEnable (GL_TEXTURE_RECTANGLE_ARB);
    }

    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    /* Hack to avoid changing to an invisible (bugged)cursor image.
     * Example: The animated Firefox cursors.
     */
    if (ci->width <= 1 && ci->height <= 1)
    {
	XFree (ci);
	return;
    }


    cursor->width = ci->width;
    cursor->height = ci->height;
    cursor->hotX = ci->xhot;
    cursor->hotY = ci->yhot;
#warning fixme: shouldnt cast malloc here. This is bound to cause trouble...
    pixels = (unsigned char *) malloc (ci->width * ci->height * 4);

    if (!pixels) 
    {
	XFree (ci);
	return;
    }

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

/* We are no longer zooming the cursor, so display it.  */
void
ZoomScreen::cursorZoomInactive ()
{
    if (!fixesSupported)
	return;

    if (cursorInfoSelected)
    {
	cursorInfoSelected = FALSE;
	XFixesSelectCursorInput (screen->dpy (), screen->root (), 0);
    }

    if (cursor.isSet)
    {
	freeCursor (&cursor);
    }

    if (cursorHidden)
    {
	cursorHidden = FALSE;
	XFixesShowCursor (screen->dpy (), screen->root ());
    }
}

/* Cursor zoom is active: We need to hide the original,
 * register for Cursor notifies and display the new one.
 * This can be called multiple times, not just on initial
 * activation.
 */
void
ZoomScreen::cursorZoomActive ()
{
    if (!fixesSupported)
	return;
    if (!optionGetScaleMouse ())
	return;

    if (!cursorInfoSelected)
    {
	cursorInfoSelected = TRUE;
        XFixesSelectCursorInput (screen->dpy (), screen->root (),
				 XFixesDisplayCursorNotifyMask);
	updateCursor (&cursor);
    }
    if (canHideCursor && !cursorHidden &&
	optionGetHideOriginalMouse ())
    {
	cursorHidden = TRUE;
	XFixesHideCursor (screen->dpy (), screen->root ());
    }
}

/* Set the zoom area
 * This is an interface for scripting. 
 * int32:x1: left x coordinate
 * int32:y1: top y coordinate
 * int32:x2: right x
 * int32:y2: bottom y 
 * x2 and y2 can be omitted to assume x1==x2+1 y1==y2+1
 * boolean:scale: True if we should modify the zoom level, false to just
 *                adjust the movement/translation.
 * boolean:restrain: True to warp the pointer so it's visible. 
 */
bool
ZoomScreen::setZoomAreaAction (CompAction         *action,
			       CompAction::State  state,
			       CompOption::Vector options)
{
	int        x1, y1, x2, y2, out;
	Bool       scale, restrain;
	CompOutput *o; 

	x1 = CompOption::getIntOptionNamed (options, "x1", -1);
	y1 = CompOption::getIntOptionNamed (options, "y1", -1);
	x2 = CompOption::getIntOptionNamed (options, "x2", -1);
	y2 = CompOption::getIntOptionNamed (options, "y2", -1);
	scale = CompOption::getBoolOptionNamed (options, "scale", FALSE);
	restrain = CompOption::getBoolOptionNamed (options, "restrain", FALSE);

	if (x1 < 0 || y1 < 0)
	    return FALSE;

	if (x2 < 0)
	    x2 = x1 + 1;

	if (y2 < 0)
	    y2 = y1 + 1;

	out = screen->outputDeviceForPoint (x1, y1);
#define WIDTH (x2 - x1)
#define HEIGHT (y2 - y1)
	setZoomArea (x1, y1, WIDTH, HEIGHT, FALSE);
	o = &screen->outputDevs (). at(out);
	if (scale && WIDTH && HEIGHT)
	    setScaleBigger (out, (float) WIDTH / o->width (), 
			    (float) HEIGHT / o->height ());
#undef WIDTH
#undef HEIGHT
	if (restrain)
	    restrainCursor (out); 

    return true;
}

/* Ensure visibility of an area defined by x1->x2/y1->y2
 * int:x1: left X coordinate
 * int:x2: right X Coordinate
 * int:y1: top Y coordinate
 * int:y2: bottom Y coordinate
 * bool:scale: zoom out if necessary to ensure visibility
 * bool:restrain: Restrain the mouse cursor
 * int:margin: The margin to use (default: 0)
 * if x2/y2 is omitted, it is ignored.
 */
bool
ZoomScreen::ensureVisibilityAction (CompAction         *action,
			            CompAction::State  state,
			            CompOption::Vector options)
{
	int        x1, y1, x2, y2, margin, out;
	Bool       scale, restrain;
	CompOutput *o;

	x1 = CompOption::getIntOptionNamed (options, "x1", -1);
	y1 = CompOption::getIntOptionNamed (options, "y1", -1);
	x2 = CompOption::getIntOptionNamed (options, "x2", -1);
	y2 = CompOption::getIntOptionNamed (options, "y2", -1);
	margin = CompOption::getBoolOptionNamed (options, "margin", 0);
	scale = CompOption::getBoolOptionNamed (options, "scale", FALSE);
	restrain = CompOption::getBoolOptionNamed (options, "restrain", FALSE);
	if (x1 < 0 || y1 < 0)
	    return FALSE;
	if (x2 < 0)
	    y2 = y1 + 1;
	out = screen->outputDeviceForPoint (x1, y1);
	ensureVisibility (x1, y1, margin);
	if (x2 >= 0 && y2 >= 0)
	    ensureVisibility (x2, y2, margin);
	o = &screen->outputDevs (). at(out);
#define WIDTH (x2 - x1)
#define HEIGHT (y2 - y1)
	if (scale && WIDTH && HEIGHT)
	    setScaleBigger (out, (float) WIDTH / o->width (), 
			    (float) HEIGHT / o->height ());
#undef WIDTH
#undef HEIGHT
	if (restrain)
	    restrainCursor (out);

	return true;
}

/* Finished here */

bool
ZoomScreen::zoomBoxActivate (CompAction         *action,
			     CompAction::State  state,
			     CompOption::Vector options)
{
	grabIndex = screen->pushGrab (None, "ezoom");
	box.setGeometry (pointerX, pointerY, 0, 0);
	if (state & CompAction::StateInitButton)
	    action->setState (action->state () | CompAction::StateTermButton);

    return true;
}

bool
ZoomScreen::zoomBoxDeactivate (CompAction         *action,
			       CompAction::State  state,
			       CompOption::Vector options)
{
	int x, y, width, height;

	if (grabIndex)
	{
	    int        out;
	    CompOutput *o;

	    screen->removeGrab (grabIndex, NULL);
	    grabIndex = 0;

	    if (pointerX < box.x1 ())
	    {
		unsigned int xB = box.x1 ();
		box.setX (pointerX);
		box.setWidth (xB - pointerX);
	    }
	    else
	    {
		box.setWidth (pointerX - box.x1 ());
	    }

	    if (pointerY < box.y1 ())
	    {
		unsigned int yB = box.y1 ();
		box.setX (pointerX);
		box.setWidth (yB - pointerY);
	    }
	    else
	    {
		box.setWidth (pointerX - box.y1 ());
	    }
	    
	    x = MIN (box.x1 (), box.x2 ());
	    y = MIN (box.y1 (), box.y2 ());
	    width = MAX (box.x1 (), box.x2 ()) - x;
	    height = MAX (box.y1 (), box.y2 ()) - y;

	    CompWindow::Geometry outGeometry (x, y, width, height, 0);

	    out = screen->outputDeviceForGeometry (outGeometry);
	    o = &screen->outputDevs (). at (out);
	    setScaleBigger (out, (float) width/o->width (), (float)
			    height/o->height ());
	    setZoomArea (x,y,width,height,FALSE);
	}

    return true;
}

/* Zoom in to the area pointed to by the mouse.
 */
bool
ZoomScreen::zoomIn (CompAction         *action,
		    CompAction::State  state,
		    CompOption::Vector options)
{
	int out = screen->outputDeviceForPoint (pointerX, pointerY);

	if (optionGetSyncMouse ()&& !isInMovement (out))
	    setCenter (pointerX, pointerY, TRUE);

	setScale (out,
		  zooms.at (out)->newZoom /
		  optionGetZoomFactor ());

    return true;
}

/* Locks down the current zoom area
 */
bool
ZoomScreen::lockZoomAction (CompAction         *action,
			    CompAction::State  state,
			    CompOption::Vector options)
{
	int out = screen->outputDeviceForPoint (pointerX, pointerY);
	zooms.at (out)->locked = !zooms.at (out)->locked;

    return true;
}

/* Zoom to a specific level.
 * target defines the target zoom level.
 * First set the scale level and mark the display as grabbed internally (to
 * catch the FocusIn event). Either target the focused window or the mouse,
 * depending on settings.
 * FIXME: A bit of a mess...
 */
bool
ZoomScreen::zoomSpecific (CompAction         *action,
			  CompAction::State  state,
			  CompOption::Vector options,
			  float		     target)
{
	int          x, y;
	int          out = screen->outputDeviceForPoint (pointerX, pointerY);
	CompWindow   *w;

	if (target == 1.0f && zooms.at (out)->newZoom == 1.0f)
	    return FALSE;
	if (screen->otherGrabExist (0))
	    return FALSE;

	setScale (out, target);

	w = screen->findWindow (screen->activeWindow ());
	if (optionGetSpecTargetFocus ()
	    && w)
	{
	    areaToWindow (w);
	}
	else
	{
	    x = CompOption::getIntOptionNamed (options, "x", 0);
	    y = CompOption::getIntOptionNamed (options, "y", 0);
	    setCenter (x, y, FALSE);
	}

    return true;
}

/* TODO: Add specific zoom boost::bind's */

/* Zooms to fit the active window to the screen without cutting
 * it off and targets it.
 */
bool
ZoomScreen::zoomToWindow (CompAction         *action,
			  CompAction::State  state,
			  CompOption::Vector options)
{
    int        width, height, out;
    Window     xid;
    CompWindow *w;
    CompOutput *o;

    xid = CompOption::getIntOptionNamed (options, "window", 0);
    w = screen->findWindow (xid);
    if (!w)
	return TRUE;
    width = w->width () + w->input ().left + w->input ().right;
    height = w->height () + w->input ().top + w->input ().bottom;
    out = screen->outputDeviceForGeometry (w->geometry ());
    o = &screen->outputDevs ().at (out);
    setScaleBigger (out, (float) width/o->width (), 
		    (float) height/o->height ());
    areaToWindow (w);
    return TRUE;
}

bool
ZoomScreen::zoomPan (CompAction         *action,
		     CompAction::State  state,
		     CompOption::Vector options,
		     float		horizAmount,
		     float		vertAmount)
{
    panZoom (horizAmount, vertAmount);

    return true;
}

/* Centers the mouse based on zoom level and translation.
 */
bool
ZoomScreen::zoomCenterMouse (CompAction         *action,
			     CompAction::State  state,
			     CompOption::Vector options)
{
    int        out;
    Window     xid;

    out = screen->outputDeviceForPoint (pointerX, pointerY);
    screen->warpPointer ((int) (screen->outputDevs ().at (out).width ()/2 +
			screen->outputDevs ().at (out).x1 () - pointerX)
			 + ((float) screen->outputDevs ().at (out).width () *
				-zooms.at (out)->xtrans),
			 (int) (screen->outputDevs ().at (out).height ()/2 +
				screen->outputDevs ().at (out).y1 () - pointerY)
			 + ((float) screen->outputDevs ().at (out).height () *
				zooms.at (out)->ytrans));
    return true;
}

/* Resize a window to fit the zoomed area.
 * This could probably do with some moving-stuff too.
 * IE: Move the zoom area afterwards. And ensure
 * the window isn't resized off-screen.
 */
bool
ZoomScreen::zoomFitWindowToZoom (CompAction         *action,
			         CompAction::State  state,
			         CompOption::Vector options)
{
    int            out;
    unsigned int   mask = CWWidth | CWHeight;
    XWindowChanges xwc;
    CompWindow     *w;

    w = screen->findWindow (CompOption::getIntOptionNamed (
							 options, "window", 0));
    if (!w)
	return TRUE;

    out = screen->outputDeviceForGeometry (w->geometry ());
    xwc.x = w->serverX ();
    xwc.y = w->serverY ();
    xwc.width = (int) (screen->outputDevs ().at (out).width () *
		       zooms.at (out)->currentZoom -
		       (int) ((w->input ().left + w->input ().right)));
    xwc.height = (int) (screen->outputDevs ().at (out).height () *
			zooms.at (out)->currentZoom -
			(int) ((w->input ().top + w->input ().bottom)));

    w->constrainNewWindowSize (xwc.width, 
			       xwc.height, 
			       &xwc.width,
			       &xwc.height);

    if (xwc.width == w->serverWidth ())
	mask &= ~CWWidth;

    if (xwc.height == w->serverHeight ())
	mask &= ~CWHeight;

    if (w->mapNum () && (mask & (CWWidth | CWHeight)))
	w->sendSyncRequest ();

    w->configureXWindow (mask, &xwc);
    return TRUE;
}

bool
ZoomScreen::initiate (CompAction         *action,
		      CompAction::State  state,
		      CompOption::Vector options)
{
    zoomIn (action, state, options);

    if (state & CompAction::StateInitKey)
	action->setState (action->state () | CompAction::StateTermKey);

    if (state & CompAction::StateInitButton)
	action->setState (action->state () | CompAction::StateTermButton);

    return true;
}

bool
ZoomScreen::zoomOut (CompAction         *action,
		     CompAction::State  state,
		     CompOption::Vector options)
{
	int out = screen->outputDeviceForPoint (pointerX, pointerY);

	setScale (out,
		  zooms.at (out)->newZoom *
		  optionGetZoomFactor ());

    return true;
}

bool
ZoomScreen::terminate (CompAction         *action,
		       CompAction::State  state,
		       CompOption::Vector options)
{
	int out;
	
	out = screen->outputDeviceForPoint (pointerX, pointerY);

	if (grabbed)
	{
	    zooms.at (out)->newZoom = 1.0f;
	    cScreen->damageScreen ();
	}

    action->setState (action->state () & ~(CompAction::StateTermKey |
					   CompAction::StateTermButton));
    return FALSE;
}

/* Focus-track related event handling.
 * The lastMapped is a hack to ensure that newly mapped windows are
 * caught even if the grab that (possibly) triggered them affected
 * the mode. Windows created by a key binding (like creating a terminal
 * on a key binding) tends to trigger FocusIn events with mode other than
 * Normal. This works around this problem.
 * FIXME: Cleanup.
 * TODO: Avoid maximized windows.
 */
void
ZoomScreen::focusTrack (XEvent *event)
{
    int           out;
    static Window lastMapped = 0;
    CompWindow    *w;

    if (event->type == MapNotify)
    {
	lastMapped = event->xmap.window;
	return;
    }
    else if (event->type != FocusIn)
	return;

    if ((event->xfocus.mode != NotifyNormal)
	&& (lastMapped != event->xfocus.window))
	return;

    lastMapped = 0;
    w = screen->findWindow (event->xfocus.window);
    if (w == NULL || w->id () == screen->activeWindow ())
	return;
 
    if (time(NULL) - lastChange < optionGetFollowFocusDelay () ||
	!optionGetFollowFocus ())
	return;

    out = screen->outputDeviceForGeometry (w->geometry ());
    if (!isActive (out) &&
	!optionGetAlwaysFocusFitWindow ())
	return;
    if (optionGetFocusFitWindow ())
    {
	int width = w->width () + w->input ().left + w->input ().right;
	int height = w->height () + w->input ().top + w->input ().bottom;
	float scale = MAX ((float) width/screen->outputDevs ().at(out).width (), 
			   (float) height/screen->outputDevs ().at (out).height ());
	if (scale > optionGetAutoscaleMin ()) 
		setScale (out, scale);
    }

    areaToWindow (w);
}

/* Event handler. Pass focus-related events on and handle XFixes events. */
void
ZoomScreen::handleEvent (XEvent *event)
{
    XMotionEvent *mev;

    switch (event->type) {
	case MotionNotify:
	    mev =  (XMotionEvent *) event;
		if (grabIndex)
		{
		    if (pointerX < box.x1 ())
		    {
			unsigned int xB = box.x1 ();
			box.setX (pointerX);
			box.setWidth (xB - pointerX);
		    }
		    else
		    {
			box.setWidth (pointerX - box.x1 ());
		    }

		    if (pointerY < box.y1 ())
		    {
			unsigned int yB = box.y1 ();
			box.setX (pointerX);
			box.setWidth (yB - pointerY);
		    }
		    else
		    {
			box.setWidth (pointerX - box.y1 ());
		    }
		    cScreen->damageScreen ();
		}
	    break;
	case FocusIn:
	case MapNotify:
	    focusTrack (event);
	    break;
	default:
	    if (event->type == fixesEventBase + XFixesCursorNotify)
	    {
		//XFixesCursorNotifyEvent *cev = (XFixesCursorNotifyEvent *)
		    //event;
		    if (cursor.isSet)
			updateCursor (&cursor);
	    }
	    break;
    }

    screen->handleEvent (event);
}

/* TODO: Use this ctor carefully */

ZoomScreen::CursorTexture::CursorTexture () :
    isSet (false)
{
}

ZoomScreen::ZoomScreen (CompScreen *screen) :
    PluginClassHandler <ZoomScreen, CompScreen> (screen),
    cScreen (CompositeScreen::get (screen)),
    gScreen (GLScreen::get (screen)),
    grabbed (0),
    grabIndex (0),
    lastChange (0),
    cursorInfoSelected (false),
    cursorHidden (false)
{
    ScreenInterface::setHandler (screen);
    CompositeScreenInterface::setHandler (cScreen);
    GLScreenInterface::setHandler (gScreen);

    int major, minor, n;
    fixesSupported = 
	XFixesQueryExtension(screen->dpy (), 
			     &fixesEventBase,
			     &fixesErrorBase);

    XFixesQueryVersion (screen->dpy (), &major, &minor);

    if (major >= 4)
	canHideCursor = true;
    else
	canHideCursor = false;

    n = screen->outputDevs ().size ();

    for (int i = 0; i < n; i++)
    {
	/* zs->grabbed is a mask ... Thus this limit */
	if (i > sizeof (long int) * 8)
	    break;
	ZoomArea *za = new ZoomArea (i);
	zooms.push_back (za);
    }

    pollHandle.setCallback (boost::bind (
				&ZoomScreen::updateMouseInterval, this, _1));

    //actionSet (InitiateInitiate, initiate);
    //actionSet (InitiateTerminate, terminate);
    optionSetZoomInButtonInitiate (boost::bind (&ZoomScreen::zoomIn, this, _1,
						_2, _3));
    optionSetZoomOutButtonInitiate (boost::bind (&ZoomScreen::zoomOut, this, _1,
						 _2, _3));
    optionSetZoomInKeyInitiate (boost::bind (&ZoomScreen::zoomIn, this, _1,
						_2, _3));
    optionSetZoomOutKeyInitiate (boost::bind (&ZoomScreen::zoomOut, this, _1,
						_2, _3));

    optionSetZoomSpecific1KeyInitiate (boost::bind (&ZoomScreen::zoomSpecific,
						    this, _1, _2, _3,
						    optionGetZoomSpec1 ()));
    optionSetZoomSpecific2KeyInitiate (boost::bind (&ZoomScreen::zoomSpecific,
						    this, _1, _2, _3,
						    optionGetZoomSpec2 ()));
    optionSetZoomSpecific3KeyInitiate (boost::bind (&ZoomScreen::zoomSpecific,
						    this, _1, _2, _3,
						    optionGetZoomSpec3 ()));

    optionSetPanLeftKeyInitiate (boost::bind (&ZoomScreen::zoomPan, this, _1,
					      _2, _3, -1, 0));
    optionSetPanRightKeyInitiate (boost::bind (&ZoomScreen::zoomPan, this, _1,
					        _2, _3, 1, 0));
    optionSetPanUpKeyInitiate (boost::bind (&ZoomScreen::zoomPan, this, _1, _2,
					     _3, 0, -1));
    optionSetPanDownKeyInitiate (boost::bind (&ZoomScreen::zoomPan, this, _1,
					       _2, _3, 0, 1));

    optionSetFitToWindowKeyInitiate (boost::bind (&ZoomScreen::zoomToWindow,
						  this, _1, _2, _3));
    optionSetCenterMouseKeyInitiate (boost::bind (&ZoomScreen::zoomCenterMouse,
						  this, _1, _2, _3));
    optionSetFitToZoomKeyInitiate (boost::bind (
					&ZoomScreen::zoomFitWindowToZoom, this,
					_1, _2, _3));


    optionSetLockZoomKeyInitiate (boost::bind (&ZoomScreen::lockZoomAction,
						this, _1, _2, _3));
    optionSetZoomBoxButtonInitiate (boost::bind (&ZoomScreen::zoomBoxActivate,
						 this, _1, _2, _3));
    optionSetZoomBoxButtonTerminate (boost::bind (
					&ZoomScreen::zoomBoxDeactivate, this,
					_1, _2, _3));
#warning: fixme: set_zoom_area has magically disappeared
    //actionSet (SetZoomAreaInitiate, setZoomAreaAction);

}

ZoomScreen::~ZoomScreen ()
{
    if (pollHandle.active ())
	pollHandle.stop ();

    if (zooms.size ())
	zooms.clear ();

    cScreen->damageScreen ();
    cursorZoomInactive ();
}

bool
ZoomPluginVTable::init ()
{
    if (!CompPlugin::checkPluginABI ("core", CORE_ABIVERSION) ||
	!CompPlugin::checkPluginABI ("composite", COMPIZ_COMPOSITE_ABI) ||
	!CompPlugin::checkPluginABI ("opengl", COMPIZ_OPENGL_ABI) ||
	!CompPlugin::checkPluginABI ("mousepoll", COMPIZ_MOUSEPOLL_ABI))
	return false;

    return true;
}
