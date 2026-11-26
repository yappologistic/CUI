package org.cui.hello;

import android.app.NativeActivity;
import android.content.Context;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/* BuildConfig is generated under android.app sub-packages with AGP 8+; the
 * generated constant class name is unstable, so read the library name from
 * the shared gradle plugin instead: NativeActivity already loads the right
 * lib for us (see lib_name meta-data) — this static load only needs the
 * same undecorated name the build selected. */

/* NativeActivity's input queue never sees IME commitText, which is how soft
 * keyboards (swipe typing in particular) deliver text, and the decor view the
 * native code shows the IME on is not a text editor, so the IME has nothing
 * to connect to. CuiInputView is a 1x1 invisible text-editor view the IME can
 * bind to; its InputConnection forwards composed text and deletions to the
 * native backend over JNI. */
public class CuiActivity extends NativeActivity {

    static {
        // the JVM must resolve nativeCommitText/nativeDeleteChars; NativeActivity
        // loads libnotes.so itself but on a classloader this activity doesn't share
        // BuildConfig.LIB_NAME is the AGP-generated native library name that
        // matches what the CMake build packaged for this app — must equal the
        // android.app.lib_name meta-data / manifest placeholder
        System.loadLibrary(BuildConfig.LIB_NAME);
    }

    private View input;
    private InputConnection ic;

    private class CuiInputView extends View {
        CuiInputView(Context c) { super(c); }

        @Override
        public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = android.text.InputType.TYPE_CLASS_TEXT;
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_NONE;
        /* kept so native editor focus changes can end compositions via
         * resetComposition(); a fresh connection would also drop span state,
         * but the IME must be told */
        InputConnection conn = new BaseInputConnection(this, false) {
            int span = 0;               /* composing-span codepoints */
            String spanText = "";       /* what we typed for it */

            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                /* a commit can also arrive while composing is still active
                 * (space after a suggestion): the prefix we typed via
                 * setComposingText is already on screen, so only the tail
                 * must be committed */
                if (text != null) {
                    String t = text.toString();
                    if (t.startsWith(spanText))
                        t = t.substring(spanText.length());
                    else if (span > 0) nativeDeleteSpanCodepoints(span);
                    if (!t.isEmpty()) nativeCommitText(t);
                }
                span = 0;
                spanText = "";
                return true;
            }

            /* The composing span (typed-but-not-final text) is diffed HERE in
             * Java: each normal keystroke extends the span by exactly one
             * char and is forwarded as a plain commit (zero lag, no
             * backspacing). Only when the IME rewrites the span (autocorrect,
             * swipe suggestion, length change) do we delete what we typed
             * before and re-type the new text. */
            @Override
            public boolean finishComposingText() {
                /* finalize: the span content is already on screen, just forget
                 * it; the inherited buffer bookkeeping must still run */
                span = 0;
                spanText = "";
                return super.finishComposingText();
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                String t = text != null ? text.toString() : "";
                int n = t.codePointCount(0, t.length());
                /* compare CONTENT: same-length content changes (swipe rewrite
                 * within one char) must still be processed; identical repeats
                 * are no-ops */
                if (!t.equals(spanText)) {
                    /* extension of what we typed (steady keystroke): only the
                     * new tail is missing on screen; a rewrite (autocorrect,
                     * swipe suggestion) deletes the old span first */
                    boolean extend = span > 0 && spanText != null && t.startsWith(spanText);
                    if (!extend && span > 0) nativeDeleteSpanCodepoints(span);
                    String tail = extend ? t.substring(spanText.length()) : t;
                    spanText = t;
                    span = n;
                    if (!tail.isEmpty()) nativeCommitText(tail);
                }
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                // counts are UTF-16 chars, nativeDeleteChars consumes
                // codepoints — convert the FULL requested range, not just the
                // part inside spanText (units before it: assume 1:1, we
                // cannot see that text from here)
                beforeLength = Math.max(0, beforeLength);
                int cut = Math.min(beforeLength, spanText.length());
                int codepoints = (beforeLength - cut)
                        + spanText.substring(spanText.length() - cut).codePointCount(0, cut);
                nativeDeleteChars(codepoints);
                span = Math.max(0, span - codepoints + (beforeLength - cut));
                if (cut > 0) spanText = spanText.substring(0, spanText.length() - cut);
                if (afterLength > 0) nativeDeleteCharsForward(afterLength);
                return true;
            }

            /* codepoint-count variant (API 33+); IMEs using it must NOT go
             * through the UTF-16 conversion above — forward counts as-is */
            @Override
            public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
                if (beforeLength == 0 && afterLength == 0) return true;
                beforeLength = Math.max(0, beforeLength);
                afterLength = Math.max(0, afterLength);
                /* before: drop the requested range off the composed text;
                 * after: text beyond the composing span is not ours to track,
                 * only the document delete is queued */
                int end = spanText.length();
                for (int i = 0; i < beforeLength && end > 0; i++)
                    end = spanText.offsetByCodePoints(end, -1);
                spanText = spanText.substring(0, end);
                span = spanText.codePointCount(0, spanText.length());
                if (beforeLength > 0) nativeDeleteChars(beforeLength);
                if (afterLength > 0) nativeDeleteCharsForward(afterLength);
                return true;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                /* soft-keyboard key events (backspace, arrows, enter) reach
                 * only this method, never the native input queue; forward
                 * them to native and swallow here (no super, or they double) */
                if (event.getAction() == KeyEvent.ACTION_DOWN)
                    nativeKeyEvent(event.getKeyCode());
                return true;
            }
        };
            ic = conn;
            return conn;
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        input = new CuiInputView(this);
        android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(1, 1);
        lp.gravity = Gravity.BOTTOM | Gravity.START;
        addContentView(input, lp);
    }

    /* native editor focus changed: finish the outstanding composition and
     * restart the IME so stale composing text cannot reach the new editor */
    public void resetComposition() {
        runOnUiThread(() -> {
            if (ic != null) ic.finishComposingText();
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.restartInput(input);
        });
    }

    /* called from the native (glue) thread; the IME must be driven on the UI thread */
    public void showKeyboard() {
        runOnUiThread(() -> {
            input.setFocusable(true);
            input.setFocusableInTouchMode(true);
            input.requestFocus();
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.showSoftInput(input, 0);
        });
    }

    public void hideKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.hideSoftInputFromWindow(input.getWindowToken(), 0);
            input.setFocusable(false);
            input.setFocusableInTouchMode(false);
            input.clearFocus();
        });
    }

    private native void nativeCommitText(String utf8);
    private native void nativeDeleteSpanCodepoints(int n);
    private native void nativeDeleteChars(int n);
    private native void nativeDeleteCharsForward(int n);
    private native void nativeKeyEvent(int code);
}
