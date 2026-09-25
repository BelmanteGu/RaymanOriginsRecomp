package io.github.belmantegu.raymanrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen Xbox 360 controller for Rayman Origins, drawn over the game.
 *
 * Left side: a floating analog stick (it re-centers where the thumb lands).
 * Right side: A (jump, hold to glide), X (attack), B, Y and RT (run).
 * Top: Back, Start and a settings button.
 *
 * The state goes to an SDL3 virtual gamepad (android_touch.cpp), so the game
 * reads it like a physical controller. Visibility, opacity and size are stored
 * in SharedPreferences and changed from the settings dialog.
 */
public class TouchControls extends View {
    // SDL_GamepadButton indices.
    static final int BTN_A = 0, BTN_B = 1, BTN_X = 2, BTN_Y = 3, BTN_BACK = 4, BTN_START = 6;
    // Not a gamepad button: the right trigger, sent as an axis.
    static final int BTN_RT = 100;
    static final int BTN_SETTINGS = 101;

    static final String PREFS = "controls";
    static final String KEY_VISIBLE = "visible";
    static final String KEY_OPACITY = "opacity";   // 10..100
    static final String KEY_SCALE = "scale";       // 60..150
    static final String KEY_FILL = "fill_screen";

    static native void nativeSetEnabled(boolean enabled);
    static native void nativeSetState(int buttons, float leftX, float leftY, float rightTrigger);

    interface SettingsListener {
        void onOpenSettings();
    }

    private static final class Button {
        final int id;
        final String label;
        final int color;
        float cx, cy, r;

        Button(int id, String label, int color) {
            this.id = id;
            this.label = label;
            this.color = color;
        }

        boolean contains(float x, float y, float slop) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= (r + slop) * (r + slop);
        }
    }

    private final Button[] buttons = {
        new Button(BTN_A, "A", 0xFF5CB85C),
        new Button(BTN_B, "B", 0xFFD9534F),
        new Button(BTN_X, "X", 0xFF428BCA),
        new Button(BTN_Y, "Y", 0xFFF0AD4E),
        new Button(BTN_RT, "RT", 0xFFBBBBBB),
        new Button(BTN_BACK, "◀", 0xFFBBBBBB),
        new Button(BTN_START, "☰", 0xFFBBBBBB),
        new Button(BTN_SETTINGS, "⚙", 0xFFBBBBBB),
    };

    private final SharedPreferences prefs;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private SettingsListener settingsListener;

    // Pointer id -> what it is holding (a Button, or STICK).
    private final SparseArray<Object> pointers = new SparseArray<>();
    private static final Object STICK = new Object();
    private float stickBaseX, stickBaseY, stickX, stickY, stickRadius;
    private boolean stickActive;
    private int pressed;         // bitmask of SDL buttons
    private boolean rtPressed;

    public TouchControls(Context context) {
        super(context);
        prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.DEFAULT_BOLD);
        setFocusable(false);
    }

    void setSettingsListener(SettingsListener listener) {
        settingsListener = listener;
    }

    /** Re-reads the preferences (after the settings dialog) and re-lays out. */
    void applyPreferences() {
        boolean visible = prefs.getBoolean(KEY_VISIBLE, true);
        nativeSetEnabled(visible);
        if (!visible) {
            releaseAll();
        }
        requestLayout();
        invalidate();
    }

    private float scale() {
        return prefs.getInt(KEY_SCALE, 100) / 100f;
    }

    private int alpha() {
        return Math.round(prefs.getInt(KEY_OPACITY, 55) * 2.55f);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutButtons(w, h);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        layoutButtons(r - l, b - t);
    }

    private void layoutButtons(int w, int h) {
        float unit = Math.min(w, h) / 1080f * scale();  // 1 unit = 1px at 1080p height
        float face = 70 * unit;                           // face button radius
        float margin = 60 * unit;
        float diamondX = w - margin - face * 3.2f;
        float diamondY = h - margin - face * 3.2f;
        float spread = face * 2.1f;
        place(BTN_A, diamondX, diamondY + spread, face);
        place(BTN_B, diamondX + spread, diamondY, face);
        place(BTN_X, diamondX - spread, diamondY, face);
        place(BTN_Y, diamondX, diamondY - spread, face);
        place(BTN_RT, diamondX - spread * 2.2f, diamondY + spread, face * 0.85f);
        float small = 42 * unit;
        place(BTN_BACK, w / 2f - small * 2.5f, margin, small);
        place(BTN_START, w / 2f + small * 2.5f, margin, small);
        place(BTN_SETTINGS, w - margin, margin, small);
        stickRadius = 150 * unit;
        if (!stickActive) {
            stickBaseX = stickX = margin + stickRadius * 1.3f;
            stickBaseY = stickY = h - margin - stickRadius * 1.3f;
        }
        text.setTextSize(face * 0.8f);
    }

    private void place(int id, float x, float y, float r) {
        for (Button b : buttons) {
            if (b.id == id) {
                b.cx = x;
                b.cy = y;
                b.r = r;
            }
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        boolean visible = prefs.getBoolean(KEY_VISIBLE, true);
        int a = alpha();
        // The settings button stays reachable (faintly) even when hidden.
        for (Button b : buttons) {
            if (!visible && b.id != BTN_SETTINGS) {
                continue;
            }
            boolean down = isDown(b.id);
            int base = visible ? a : Math.min(a, 60);
            fill.setColor(b.color);
            fill.setAlpha(down ? Math.min(255, base + 90) : base / 2);
            canvas.drawCircle(b.cx, b.cy, b.r, fill);
            stroke.setColor(Color.WHITE);
            stroke.setAlpha(base);
            stroke.setStrokeWidth(b.r * 0.08f);
            canvas.drawCircle(b.cx, b.cy, b.r, stroke);
            text.setColor(Color.WHITE);
            text.setAlpha(base);
            float size = text.getTextSize();
            if (b.r < 50) {
                text.setTextSize(b.r * 1.1f);
            }
            canvas.drawText(b.label, b.cx, b.cy - (text.descent() + text.ascent()) / 2, text);
            text.setTextSize(size);
        }
        if (!visible) {
            return;
        }
        // Stick: base ring and knob.
        stroke.setColor(Color.WHITE);
        stroke.setAlpha(a);
        stroke.setStrokeWidth(stickRadius * 0.04f);
        canvas.drawCircle(stickBaseX, stickBaseY, stickRadius, stroke);
        fill.setColor(Color.WHITE);
        fill.setAlpha(stickActive ? Math.min(255, a + 60) : a / 2);
        canvas.drawCircle(stickX, stickY, stickRadius * 0.42f, fill);
    }

    private boolean isDown(int id) {
        if (id == BTN_RT) {
            return rtPressed;
        }
        if (id == BTN_SETTINGS) {
            return false;
        }
        return (pressed & (1 << id)) != 0;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        boolean visible = prefs.getBoolean(KEY_VISIBLE, true);
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(index);
                float x = e.getX(index), y = e.getY(index);
                Button hit = hitTest(x, y);
                if (hit != null && hit.id == BTN_SETTINGS) {
                    if (settingsListener != null) {
                        settingsListener.onOpenSettings();
                    }
                    return true;
                }
                if (!visible) {
                    return false;
                }
                if (hit != null) {
                    pointers.put(id, hit);
                } else if (x < getWidth() * 0.45f) {
                    // Floating stick: re-center under the thumb.
                    pointers.put(id, STICK);
                    stickActive = true;
                    stickBaseX = stickX = x;
                    stickBaseY = stickY = y;
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                if (!visible) {
                    return false;
                }
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    Object held = pointers.get(id);
                    float x = e.getX(i), y = e.getY(i);
                    if (held == STICK) {
                        float dx = x - stickBaseX, dy = y - stickBaseY;
                        float len = (float) Math.hypot(dx, dy);
                        if (len > stickRadius) {
                            dx *= stickRadius / len;
                            dy *= stickRadius / len;
                        }
                        stickX = stickBaseX + dx;
                        stickY = stickBaseY + dy;
                    } else if (held != null) {
                        // Slide between buttons (e.g. from A onto X).
                        Button hit = hitTest(x, y);
                        if (hit != null && hit.id != BTN_SETTINGS) {
                            pointers.put(id, hit);
                        }
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(index);
                if (pointers.get(id) == STICK) {
                    stickActive = false;
                    layoutButtons(getWidth(), getHeight());
                }
                pointers.remove(id);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
            default:
                return true;
        }
        publish();
        invalidate();
        return true;
    }

    private Button hitTest(float x, float y) {
        Button best = null;
        float bestDist = Float.MAX_VALUE;
        for (Button b : buttons) {
            // A little slop so near misses still land on the closest button.
            if (b.contains(x, y, b.r * 0.25f)) {
                float d = (float) Math.hypot(x - b.cx, y - b.cy);
                if (d < bestDist) {
                    bestDist = d;
                    best = b;
                }
            }
        }
        return best;
    }

    private void releaseAll() {
        pointers.clear();
        stickActive = false;
        layoutButtons(getWidth(), getHeight());
        publish();
    }

    private void publish() {
        int mask = 0;
        boolean rt = false;
        boolean stick = false;
        for (int i = 0; i < pointers.size(); i++) {
            Object held = pointers.valueAt(i);
            if (held == STICK) {
                stick = true;
            } else if (held instanceof Button) {
                Button b = (Button) held;
                if (b.id == BTN_RT) {
                    rt = true;
                } else if (b.id != BTN_SETTINGS) {
                    mask |= 1 << b.id;
                }
            }
        }
        pressed = mask;
        rtPressed = rt;
        stickActive = stick;
        float lx = 0, ly = 0;
        if (stick && stickRadius > 0) {
            lx = (stickX - stickBaseX) / stickRadius;
            ly = (stickY - stickBaseY) / stickRadius;
        }
        nativeSetState(mask, lx, ly, rt ? 1f : 0f);
    }
}
