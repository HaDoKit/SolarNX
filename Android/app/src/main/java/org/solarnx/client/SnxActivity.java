package org.solarnx.client;

import android.app.AlertDialog;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.text.InputType;
import android.widget.EditText;

import org.libsdl.app.SDLActivity;

// SolarNX entry activity. Extends SDL's activity so libSDL2.so is loaded as usual; the only
// addition is a standard Android text-edit dialog requested by the UI shell (the shell's
// built-in SDL text input loses touch focus after the soft keyboard is dismissed and a
// floating keyboard covers the field being edited).
public class SnxActivity extends SDLActivity {

    private native void nativeTextResult(String rowKey, String text);

    // Called from the SDL thread via JNI when a settings row needs text input.
    public void showTextDialog(final String rowKey, String label, String current, String hint) {
        runOnUiThread(() -> {
            final EditText input = new EditText(this);
            input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            input.setSingleLine(true);
            // Landscape shows the standard extracted editor: a single input line on
            // top (with our hint) and the docked keyboard below - no dialog chrome.
            input.setImeOptions(EditorInfo.IME_ACTION_DONE);
            input.setText(current);
            input.setSelectAllOnFocus(true);
            input.setHint(hint); // explains what the field expects while it is empty

            final AlertDialog dlg = new AlertDialog.Builder(this)
                    .setTitle(label)
                    .setView(input)
                    .setPositiveButton(android.R.string.ok,
                            (d, which) -> nativeTextResult(rowKey, input.getText().toString()))
                    .setNegativeButton(android.R.string.cancel,
                            (d, which) -> nativeTextResult(rowKey, null))
                    .setOnCancelListener(d -> nativeTextResult(rowKey, null))
                    .create();
            // The keyboard's own Done key commits - the standard Android pattern, and it also
            // works when a floating keyboard covers the dialog buttons.
            input.setOnEditorActionListener((v, actionId, ev) -> {
                if (actionId == EditorInfo.IME_ACTION_DONE) {
                    dlg.dismiss();
                    nativeTextResult(rowKey, v.getText().toString());
                    return true;
                }
                return false;
            });
            Window w = dlg.getWindow();
            if (w != null)
                // ADJUST_PAN slides the dialog above the IME so the field stays visible.
                w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE |
                                   WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN);
            dlg.show();
            input.postDelayed(() -> {
                input.requestFocus();
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                imm.showSoftInput(input, InputMethodManager.SHOW_IMPLICIT);
            }, 150);
        });
    }
}
