package com.beer.loader;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen Xbox controller, drawn over the game. Each finger that lands on a control drives it (sticks move relative
 * to where the thumb landed); fingers that land elsewhere go to the touchpad. The state is sent to the native virtual
 * controller (platform/beer_gamepad.c) on every change. The small round button at the top hides and shows the pad.
 */
public class PadView extends View {
    static native void nativePad(boolean connected, int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native void nativeTouch(int action, float x, float y, int pointers, long timeNs);

    // XINPUT_GAMEPAD bits
    static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20, LS = 0x40, RS = 0x80,
            LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    static final int LT = 0x10000, RT = 0x20000;   // triggers, as pseudo-buttons
    static final int LSTICK = -1, RSTICK = -2, TOGGLE = -3, TOUCHPAD = -4;

    static class Control {
        final int bits; final String label; final float cx, cy, r;   // centre and radius as fractions (r of height)
        Control(int bits, String label, float cx, float cy, float r) {
            this.bits = bits; this.label = label; this.cx = cx; this.cy = cy; this.r = r;
        }
    }

    static final Control[] CONTROLS = {
        new Control(LSTICK, "", 0.13f, 0.72f, 0.17f), new Control(RSTICK, "", 0.66f, 0.80f, 0.14f),
        new Control(A, "A", 0.885f, 0.74f, 0.065f), new Control(B, "B", 0.945f, 0.60f, 0.065f),
        new Control(X, "X", 0.825f, 0.60f, 0.065f), new Control(Y, "Y", 0.885f, 0.46f, 0.065f),
        new Control(UP, "▲", 0.13f, 0.30f, 0.055f), new Control(DOWN, "▼", 0.13f, 0.46f, 0.055f),
        new Control(LEFT, "◀", 0.085f, 0.38f, 0.055f), new Control(RIGHT, "▶", 0.175f, 0.38f, 0.055f),
        new Control(LB, "LB", 0.05f, 0.10f, 0.06f), new Control(LT, "LT", 0.13f, 0.10f, 0.06f),
        new Control(RB, "RB", 0.95f, 0.10f, 0.06f), new Control(RT, "RT", 0.87f, 0.10f, 0.06f),
        new Control(BACK, "⧉", 0.43f, 0.08f, 0.045f), new Control(START, "☰", 0.57f, 0.08f, 0.045f),
        new Control(LS, "L3", 0.27f, 0.90f, 0.045f), new Control(RS, "R3", 0.53f, 0.90f, 0.045f),
        new Control(TOGGLE, "◎", 0.50f, 0.03f, 0.03f),
    };

    final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), ring = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    final int[] pointerControl = new int[16];        // control index per pointer id, or TOUCHPAD, or -100 = none
    final float[] originX = new float[16], originY = new float[16];
    int pressed;                                     // buttons | LT | RT
    float lx, ly, rx, ry;                            // -1..1, +y down (screen)
    boolean hidden;

    public PadView(Context context) {
        super(context);
        fill.setStyle(Paint.Style.FILL);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(3);
        ring.setColor(Color.argb(150, 255, 255, 255));
        text.setColor(Color.argb(200, 255, 255, 255));
        text.setTextAlign(Paint.Align.CENTER);
        java.util.Arrays.fill(pointerControl, -100);
        send();
    }

    float px(Control c) { return c.cx * getWidth(); }
    float py(Control c) { return c.cy * getHeight(); }
    float pr(Control c) { return c.r * getHeight(); }

    int hit(float x, float y) {
        for (int i = 0; i < CONTROLS.length; ++i) {
            Control c = CONTROLS[i];
            if (hidden && c.bits != TOGGLE) continue;
            float dx = x - px(c), dy = y - py(c), r = pr(c) * (c.bits == LSTICK || c.bits == RSTICK ? 1.25f : 1.15f);
            if (dx * dx + dy * dy <= r * r) return i;
        }
        return -100;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        for (Control c : CONTROLS) {
            if (hidden && c.bits != TOGGLE) continue;
            float x = px(c), y = py(c), r = pr(c);
            boolean down = c.bits > 0 && (pressed & c.bits) != 0;
            fill.setColor(down ? Color.argb(140, 255, 255, 255) : Color.argb(55, 0, 0, 0));
            if (c.bits == LSTICK || c.bits == RSTICK) {
                canvas.drawCircle(x, y, r, fill);
                canvas.drawCircle(x, y, r, ring);
                float sx = c.bits == LSTICK ? lx : rx, sy = c.bits == LSTICK ? ly : ry;
                fill.setColor(Color.argb(150, 255, 255, 255));
                canvas.drawCircle(x + sx * r * 0.6f, y + sy * r * 0.6f, r * 0.4f, fill);
                continue;
            }
            canvas.drawCircle(x, y, r, fill);
            canvas.drawCircle(x, y, r, ring);
            text.setTextSize(r * 0.8f);
            canvas.drawText(c.label, x, y + r * 0.28f, text);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked(), index = e.getActionIndex(), id = e.getPointerId(index) & 15;
        long time = e.getEventTime() * 1000000L;
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(index), y = e.getY(index);
                int control = hit(x, y);
                if (control >= 0 && CONTROLS[control].bits == TOGGLE) {
                    hidden = !hidden;
                    pressed = 0; lx = ly = rx = ry = 0;
                    pointerControl[id] = -100;
                } else if (control >= 0) {
                    pointerControl[id] = control;
                    originX[id] = x; originY[id] = y;
                    if (CONTROLS[control].bits > 0) pressed |= CONTROLS[control].bits;
                } else {
                    pointerControl[id] = TOUCHPAD;
                    nativeTouch(touchpadFingers() == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN, x, y,
                            touchpadFingers(), time);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    int pid = e.getPointerId(i) & 15, control = pointerControl[pid];
                    if (control == TOUCHPAD) {
                        nativeTouch(MotionEvent.ACTION_MOVE, e.getX(i), e.getY(i), touchpadFingers(), time);
                    } else if (control >= 0 && (CONTROLS[control].bits == LSTICK || CONTROLS[control].bits == RSTICK)) {
                        float r = pr(CONTROLS[control]);
                        float dx = (e.getX(i) - originX[pid]) / r, dy = (e.getY(i) - originY[pid]) / r;
                        float len = (float) Math.sqrt(dx * dx + dy * dy);
                        if (len > 1) { dx /= len; dy /= len; }
                        if (CONTROLS[control].bits == LSTICK) { lx = dx; ly = dy; } else { rx = dx; ry = dy; }
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    if (action != MotionEvent.ACTION_CANCEL && i != index) continue;
                    int pid = e.getPointerId(i) & 15, control = pointerControl[pid];
                    if (control == TOUCHPAD) {
                        int fingers = touchpadFingers();
                        nativeTouch(fingers == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP, e.getX(i), e.getY(i),
                                fingers, time);
                    } else if (control >= 0) {
                        int bits = CONTROLS[control].bits;
                        if (bits > 0) pressed &= ~bits;
                        else if (bits == LSTICK) lx = ly = 0;
                        else if (bits == RSTICK) rx = ry = 0;
                    }
                    pointerControl[pid] = -100;
                }
                break;
            default:
                return true;
        }
        send();
        invalidate();
        return true;
    }

    int touchpadFingers() {
        int n = 0;
        for (int c : pointerControl) if (c == TOUCHPAD) ++n;
        return n;
    }

    static int axis(float v) { return Math.round(Math.max(-1f, Math.min(1f, v)) * 32767); }

    void send() {
        nativePad(!hidden, pressed & 0xFFFF, (pressed & LT) != 0 ? 255 : 0, (pressed & RT) != 0 ? 255 : 0,
                axis(lx), axis(-ly), axis(rx), axis(-ry));
    }
}
